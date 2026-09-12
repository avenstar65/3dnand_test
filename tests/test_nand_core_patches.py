#!/usr/bin/env python3
"""Executable patch application and actual core helper regression tests."""
import pathlib
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = pathlib.Path(os.environ.get('NAND_CORE_SOURCE', ROOT / 'work/linux/linux-7.0.12'))
FILES = ['drivers/mtd/nand/raw/internals.h', 'drivers/mtd/nand/raw/nand_base.c',
         'drivers/mtd/nand/raw/nand_bbt.c', 'include/linux/mtd/rawnand.h']


def run(*args, **kwargs):
    return subprocess.run(args, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, **kwargs)


def function(text, name):
    match = re.search(r'^(?:static (?:inline )?)?(?:int|void|bool|u64|u32|uint8_t) ' + name + r'\(', text, re.M)
    if not match:
        raise AssertionError('missing core function: ' + name)
    start = text.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;
typedef int64_t loff_t;
#define BIT(n) (1U << (n))
#define NAND_NON_POWER_OF_2_GEOMETRY BIT(15)
#define NAND_MARKBAD_NO_ERASE BIT(28)
#define NAND_BBT_NO_OOB_BBM BIT(4)
#define NAND_BBT_USE_FLASH BIT(0)
#define BBT_BLOCK_GOOD 0
#define BBT_BLOCK_WORN 1
#define BBT_BLOCK_RESERVED 2
#define BBT_BLOCK_FACTORY_BAD 3
#define BBT_ENTRY_SHIFT 2
#define BBT_ENTRY_MASK 3
#define pr_debug(...) ((void)0)
#define div_u64(a,b) ((u64)(a)/(b))
#define div64_u64(a,b) ((u64)(a)/(b))
#define WARN_ON_ONCE(x) (x)
struct nand_chip;
struct mtd_info { u64 size; u32 writesize, erasesize; struct {unsigned badblocks;} ecc_stats; struct nand_chip *chip; };
struct nand_chip { unsigned options, bbt_options; unsigned page_shift,phys_erase_shift,chip_shift,pagemask,subpagesize; int cur_cs; unsigned char *bbt; struct {u64 size; unsigned ntargets;} base; struct mtd_info mtd; struct {void (*sync)(struct nand_chip *);} ops; };
static struct mtd_info *nand_to_mtd(struct nand_chip *c) {return &c->mtd;}
static struct nand_chip *mtd_to_nand(struct mtd_info *m) {return m->chip;}
#define nanddev_target_size(b) ((b)->size)
#define nanddev_ntargets(b) ((b)->ntargets)
struct erase_info {u64 addr,len;};
static int locked, erases, markers, marker_error;
static void nand_get_device(struct nand_chip *c) {(void)c; assert(!locked); locked=1;}
static void nand_release_device(struct nand_chip *c) {(void)c; assert(locked); locked=0;}
static int nand_erase_nand(struct nand_chip *c,struct erase_info *e,int a) {(void)a; assert(e->len==c->mtd.erasesize); erases++; return 0;}
static int nand_markbad_bbm(struct nand_chip *c,loff_t o) {(void)c;(void)o;assert(locked);markers++;return marker_error;}
static int nand_markbad_bbt(struct nand_chip *c,loff_t o) {(void)c;(void)o;return 0;}
'''


class CorePatches(unittest.TestCase):
    def test_real_core_geometry_and_hooks(self):
        with tempfile.TemporaryDirectory() as directory:
            tree = pathlib.Path(directory)
            for name in FILES:
                target = tree / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(SOURCE / name, target)
            applied = run('sh', str(ROOT / 'scripts/apply-linux-core-patches.sh'), str(tree))
            self.assertEqual(applied.returncode, 0, applied.stdout)
            base = (tree / FILES[1]).read_text()
            internals = (tree / FILES[0]).read_text()
            bbt = (tree / FILES[2]).read_text()
        self.assertIn('targetsize / mtd->writesize > (u64)U16_MAX + 1', base)
        self.assertFalse(65536 > 65535 + 1)
        self.assertTrue(65537 > 65535 + 1)
        helpers = '\n'.join(function(internals, n) for n in re.findall(r'static inline (?:bool|u64|u32) (nand_\w+)\(', internals)
                            if n.startswith(('nand_has_non_', 'nand_is_subpage_', 'nand_page_', 'nand_offs_', 'nand_pages_', 'nand_eraseblock_')))
        code = PRELUDE + helpers + '\n' + '\n'.join(function(base, n) for n in ['check_offs_len','nand_sync','nand_block_markbad_lowlevel'])
        code += '\n' + '\n'.join(function(bbt, n) for n in ['bbt_get_entry','bbt_mark_entry','nand_bbt_markbad_from_oob'])
        code += r'''
static void sync_hook(struct nand_chip *c) {(void)c;assert(locked);markers++;}
int main(void) {
 struct nand_chip c={0}; c.mtd.chip=&c; c.options=NAND_NON_POWER_OF_2_GEOMETRY;
 c.mtd.writesize=49152; c.mtd.erasesize=78643200; c.subpagesize=49152; c.phys_erase_shift=20;
 assert(nand_is_subpage_aligned(&c,49152));
 assert(!nand_is_subpage_aligned(&c,16384));
 assert(check_offs_len(&c,78643200,78643200)==0);
 assert(check_offs_len(&c,78643201,78643200)==-EINVAL);
 assert(check_offs_len(&c,1048576,1048576)==-EINVAL);
 assert(check_offs_len(&c,0,78643199)==-EINVAL);
 nand_sync(&c.mtd); assert(markers==0 && !locked);
 c.ops.sync=sync_hook; nand_sync(&c.mtd); assert(markers==1 && !locked);
 markers=0; c.options|=NAND_MARKBAD_NO_ERASE;
 assert(nand_block_markbad_lowlevel(&c,0)==0); assert(erases==0 && markers==1);
 c.options &= ~NAND_MARKBAD_NO_ERASE;
 assert(nand_block_markbad_lowlevel(&c,0)==0); assert(erases==1 && markers==2);
 c.bbt_options=NAND_BBT_NO_OOB_BBM;
 assert(nand_block_markbad_lowlevel(&c,0)==0); assert(erases==1 && markers==2);
 c.bbt_options=0; c.base.size=235929600; c.base.ntargets=2; c.cur_cs=1;
 assert(nand_offs_to_target(&c,235929599)==0);
 assert(nand_offs_to_target(&c,235929600)==1);
 assert(nand_page_in_target(&c,4800)==0);
 assert(nand_page_in_target(&c,4799)==4799);
 assert(nand_offs_to_page(&c,78643199)==1599);
 assert(nand_offs_in_page(&c,78643199)==49151);
 assert(nand_offs_in_page(&c,78643200)==0);
 assert(nand_page_to_eraseblock(&c,1600)==1);
 assert(nand_eraseblock_to_page(&c,2)==3200);
 assert(nand_page_to_offs(&c,4800)==235929600);
 assert(nand_bbt_markbad_from_oob(&c,1600)==0); /* scan: no RAM BBT yet */
 unsigned char table[2]={0}; c.bbt=table; c.mtd.ecc_stats.badblocks=0;
 assert(nand_bbt_markbad_from_oob(&c,1600)==0); /* target 1, block 1 -> global 4 */
 assert(table[0]==0 && table[1]==1 && c.mtd.ecc_stats.badblocks==1);
 assert(nand_bbt_markbad_from_oob(&c,1601)==0 && c.mtd.ecc_stats.badblocks==1);
 assert(nand_bbt_markbad_from_oob(&c,4800)==-EINVAL);
 c.cur_cs=-1; assert(nand_bbt_markbad_from_oob(&c,0)==-EINVAL);
 c.cur_cs=0; c.bbt_options=NAND_BBT_USE_FLASH;
 assert(nand_bbt_markbad_from_oob(&c,0)==-EOPNOTSUPP && table[0]==0);
 c.mtd.writesize=16384; c.mtd.erasesize=22937600; c.base.size=68812800;
 assert(nand_offs_to_page(&c,22937599)==1399);
 assert(nand_offs_in_page(&c,22937599)==16383);
 assert(nand_page_to_eraseblock(&c,1400)==1);
 assert(nand_page_in_target(&c,4200)==0);
 c.mtd.erasesize=26214400; c.base.size=78643200;
 assert(nand_page_to_eraseblock(&c,1600)==1);
 assert(nand_offs_to_eraseblock(&c,78643199)==2);
 c.options=0; c.mtd.writesize=2048; c.mtd.erasesize=131072; c.subpagesize=2048;
 assert(nand_is_subpage_aligned(&c,4096));
 c.page_shift=11; c.phys_erase_shift=17; c.chip_shift=27; c.pagemask=65535;
 assert(nand_offs_to_page(&c,131071)==63);
 assert(nand_offs_in_page(&c,131071)==2047);
 assert(nand_page_in_target(&c,65536)==0);
 assert(nand_offs_to_target(&c,134217728)==1);
 assert(nand_eraseblock_to_page(&c,1)==64);
 assert(check_offs_len(&c,131072,131072)==0);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'test.c'
            binary = pathlib.Path(directory) / 'test'
            source.write_text(code)
            compiled = run('cc', '-std=c11', '-g', '-fsanitize=address,undefined', '-Werror', '-Wno-unused-function', str(source), '-o', str(binary))
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            result = run(str(binary))
            self.assertEqual(result.returncode, 0, result.stdout)

    def test_application_is_atomic_and_repeatable(self):
        script = ROOT / 'scripts/apply-linux-core-patches.sh'
        self.assertTrue(script.exists(), 'core patch application entry point is missing')
        with tempfile.TemporaryDirectory() as directory:
            tree = pathlib.Path(directory)
            for name in FILES:
                target = tree / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(SOURCE / name, target)
            first = run('sh', str(script), str(tree))
            self.assertEqual(first.returncode, 0, first.stdout)
            expected = {name: (tree / name).read_bytes() for name in FILES}
            again = run('sh', str(script), str(tree))
            self.assertEqual(again.returncode, 0, again.stdout)
            self.assertEqual(expected, {name: (tree / name).read_bytes() for name in FILES})
            # A changed covered file must fail before modifying any other file.
            target = tree / FILES[1]
            target.write_text(target.read_text().replace('nand_sync', 'foreign_sync'))
            before = {name: (tree / name).read_bytes() for name in FILES}
            broken = run('sh', str(script), str(tree))
            self.assertNotEqual(broken.returncode, 0, broken.stdout)
            self.assertEqual(before, {name: (tree / name).read_bytes() for name in FILES})


if __name__ == '__main__':
    unittest.main()
