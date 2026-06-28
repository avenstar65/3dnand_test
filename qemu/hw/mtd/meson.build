# Overlay snippet for a QEMU source tree.
#
# Copy q3n-nand.c into QEMU's hw/mtd/ directory and add this line to the
# existing hw/mtd/meson.build.

system_ss.add(when: 'CONFIG_Q3N_NAND', if_true: files('q3n-nand.c'))
