# Makefile for the Linux graphics to console drivers.
# 5 Aug 1999, James Simmons, <mailto:jsimmons@users.sf.net>
# Rewritten to use lists instead of if-statements.

# Font handling
font-objs := fonts.o

font-objs-$(CONFIG_FONT_SUN8x16)   += font_sun8x16.o
font-objs-$(CONFIG_FONT_SUN12x22)  += font_sun12x22.o
font-objs-$(CONFIG_FONT_8x8)       += font_8x8.o
font-objs-$(CONFIG_FONT_8x16)      += font_8x16.o
font-objs-$(CONFIG_FONT_6x11)      += font_6x11.o
font-objs-$(CONFIG_FONT_PEARL_8x8) += font_pearl_8x8.o
font-objs-$(CONFIG_FONT_ACORN_8x8) += font_acorn_8x8.o
font-objs-$(CONFIG_FONT_MINI_4x6)  += font_mini_4x6.o

font-objs += $(font-objs-y)

# Each configuration option enables a list of files.

obj-$(CONFIG_DUMMY_CONSOLE)       += dummycon.o
obj-$(CONFIG_SGI_NEWPORT_CONSOLE) += newport_con.o
obj-$(CONFIG_PROM_CONSOLE)        += promcon.o promcon_tbl.o
obj-$(CONFIG_STI_CONSOLE)         += sticon.o sticore.o
obj-$(CONFIG_VGA_CONSOLE)         += vgacon.o
obj-$(CONFIG_MDA_CONSOLE)         += mdacon.o
obj-$(CONFIG_FRAMEBUFFER_CONSOLE) += fbcon.o font.o

obj-$(CONFIG_FB_STI)              += sticore.o

# Targets that kbuild needs to know about
targets := promcon_tbl.c

quiet_cmd_conmakehash = CNMKHSH $@
      cmd_conmakehash = scripts/conmakehash $< | \
		sed -e '/\#include <[^>]*>/p' -e 's/types/init/' \
		-e 's/dfont\(_uni.*\]\)/promfont\1 __initdata/' > $@

$(obj)/promcon_tbl.c: $(src)/prom.uni FORCE
	$(call if_changed,conmakehash)
