#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <cstdint>
#include <windows.h>
#include <commctrl.h>
#include <vfw.h>
#include <wil/resource.h>  // wil::unique_handle, unique_hbitmap, unique_hbrush, unique_hpen, unique_hdc, unique_hhook

#include "cache2.h"
#include "config2.h"
#include "filter2.h"
#include "input2.h"
#include "logger2.h"
#include "module2.h"
#include "output2.h"
#include "plugin2.h"
