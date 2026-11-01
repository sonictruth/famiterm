; 32KB MMC1 CHR-ROM as eight 4KB banks.  Bank 0: colour splash (see scripts/gen-splash.py),
; banks 1-7: fonts, in the order of FONT_NAMES in the Makefile and font_names[] in
; romcart/screen.c.  The font is mapped at pattern table 0, the splash at pattern table 1.
; D-pad left/right cycle the fonts.  Kept to 32KB: the old EverDrive N8 reads garbage
; from MMC1 CHR banks past 32KB.
.section .chr_rom,"a",@progbits
.incbin "romcart/chr/splash.chr"
.incbin "romcart/chr/font_Altered_Chrome.chr"
.incbin "romcart/chr/font_Around.chr"
.incbin "romcart/chr/font_Bauhaus.chr"
.incbin "romcart/chr/font_Computing_60s.chr"
.incbin "romcart/chr/font_Security_Concepts.chr"
.incbin "romcart/chr/font_ZX_Chicago.chr"
.incbin "romcart/chr/font_ZX_OCR-A.chr"
