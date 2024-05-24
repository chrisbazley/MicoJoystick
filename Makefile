# Project:   MicoJSMod


# Toolflags:
CCflags = -c -depend !Depend -IC: -throwback -zM -ff -zps1 -Ospace -apcs 3/26/fpe2/swst/fp/fpr 
C++flags = -c -depend !Depend -IC: -throwback
Linkflags = -rmf -c++ -o $@ 
ObjAsmflags = -throwback -NoCache -depend !Depend
CMHGflags = -depend !Depend -throwback -IC: -26bit 
LibFileflags = -c -o $@
Squeezeflags = -o $@
ASMflags = -processor ARM7 -throwback -apcs R 


# Final targets:
@.MicoJoystick:   @.o.MicoJoyHdr @.o.MicoJoy C:o.stubs26 @.o.errors 
        Link $(Linkflags) @.o.MicoJoyHdr @.o.MicoJoy C:o.stubs26 @.o.errors 


# User-editable dependencies:
@.h.MicoJoyHdr:   @.cmhg.MicoJoyHdr
        cmhg $(cmhgflags) @.cmhg.MicoJoyHdr -d @.h.MicoJoyHdr

# Static dependencies:
@.o.MicoJoyHdr:   @.cmhg.MicoJoyHdr
        cmhg $(cmhgflags) @.cmhg.MicoJoyHdr -o @.o.MicoJoyHdr
@.o.MicoJoy:   @.c.MicoJoy
        cc $(ccflags) -o @.o.MicoJoy @.c.MicoJoy 
@.o.errors:   @.a.errors
        ASM $(ASMFlags) -output @.o.errors @.a.errors


# Dynamic dependencies:
o.MicoJoy:	c.MicoJoy
o.MicoJoy:	C:h.kernel
o.MicoJoy:	C:h.swis
o.MicoJoy:	C:h.syslog
o.MicoJoy:	C:h.kernel
o.MicoJoy:	h.MicoJoyHdr
o.MicoJoy:	h.MicoJoyErr
o.MicoJoy:	C:h.kernel
