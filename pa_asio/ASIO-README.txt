ASIO-README.txt

This document contains information to help you compile PortAudio with 
ASIO support. If you find any omissions or errors in this document 
please notify Ross Bencina <rossb@audiomulch.com>.



Obtaining the ASIO SDK
----------------------

In order to build PortAudio with ASIO support, you need to download 
the ASIO SDK (version 2.0) from Steinberg. Steinberg makes the ASIO 
SDK available to anyone free of charge, however they do not permit its 
source code to be distributed.

NOTE: In some cases the ASIO SDK requires patching to work with your 
compiler, see below for further details.

http://www.steinberg.net/en/ps/support/3rdparty/asio_sdk/

If the above link is broken search Google for:
"download steinberg ASIO SDK"



Building the ASIO SDK on Windows
--------------------------------

To build the ASIO SDK on Windows you need to compile and link with the 
following files from the ASIO SDK:

asio_sdk\common\asio.cpp *
asio_sdk\host\asiodrivers.cpp
asio_sdk\host\pc\asiolist.cpp

You may also need to adjust your include paths to support inclusion of 
header files from the above directories.

* - if you're not using a Microsoft compiler you may need to use a 
patched version of asio.cpp, see below.



Non-Microsoft (MSVC) Compilers on Windows including Borland and GCC
-------------------------------------------------------------------

Steinberg did not specify a calling convention in the IASIO interface 
definition. This causes the Microsoft compiler to use the proprietary 
thiscall convention which is not compatible with other compilers, such 
as compilers from Borland (BCC and C++Builder) and GNU (gcc). 
Steinberg's ASIO SDK will compile but crash on initialization if 
compiled with a non-Microsoft compiler on Windows.

You can get more information about the problem, and download a patch 
to compile the ASIO SDK with non-Microsoft compilers from the 
following page:
http://www.audiomulch.com/~rossb/code/calliasio



Macintosh ASIO SDK Bug Patch
----------------------------

There is a bug in the ASIO SDK that causes the Macintosh version to 
often fail during initialization. Below is a patch that you can apply.

In codefragments.cpp replace getFrontProcessDirectory function with 
the following one (GetFrontProcess replaced by GetCurrentProcess).


bool CodeFragments::getFrontProcessDirectory(void *specs)
{
	FSSpec *fss = (FSSpec *)specs;
	ProcessInfoRec pif;
	ProcessSerialNumber psn;

	memset(&psn,0,(long)sizeof(ProcessSerialNumber));
	//  if(GetFrontProcess(&psn) == noErr)  // wrong !!!
	if(GetCurrentProcess(&psn) == noErr)  // correct !!!
	{
		pif.processName = 0;
		pif.processAppSpec = fss;
		pif.processInfoLength = sizeof(ProcessInfoRec);
		if(GetProcessInformation(&psn, &pif) == noErr)
				return true;
	}
	return false;
}


---
