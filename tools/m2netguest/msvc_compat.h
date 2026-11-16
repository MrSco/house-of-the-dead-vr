// m2netguest: main's src/net C files under MSVC (forced in with /FI). GCC's attributes there are format hints only.
#pragma once
#ifdef _MSC_VER
#define __attribute__(x)
#endif
