NAME = OHCI_reloc
PROJECTVERSION = 1.1
LANGUAGE = English
CLASSES = OHCIController.m OHCIUSBKeyboard.m OHCIUSBPointer.m
CFILES = OHCICore.c OHCIInterruptState.c OHCIPCI.c OHCIInputQueue.c OHCICompletionWait.c USBCore.c USBHID.c
HFILES = OHCIController.h OHCICore.h OHCIRegs.h OHCITypes.h OHCIMemory.h OHCIUSBNames.h OHCIInterruptState.h OHCIPCI.h OHCIInputQueue.h OHCICompletionWait.h OHCIUSBKeyboard.h OHCIUSBPointer.h OHCIVersion.h OHCIRuntime.h OpenStepInputCompat.h USBCore.h USBHID.h
OTHERSRCS = OHCIEnumeration.inc Makefile.preamble Makefile Makefile.postamble Load_Commands.sect
MAKEFILEDIR = /NextDeveloper/Makefiles/app
MAKEFILE = tool.make
SOURCEMODE = 444
LIBS =
DEBUG_LIBS = $(LIBS)
PROF_LIBS = $(LIBS)
-include Makefile.preamble
include $(MAKEFILEDIR)/$(MAKEFILE)
-include Makefile.postamble
-include Makefile.dependencies
