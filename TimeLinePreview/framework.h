#pragma once

#define WIN32_LEAN_AND_MEAN             // Windows ヘッダーからほとんど使用されていない部分を除外する
#define NOMINMAX
// Windows ヘッダー ファイル
#include <cstdint>
#include <windows.h>
#include <commctrl.h>
#include <vfw.h>

#include "cache2.h"
#include "config2.h"
#include "filter2.h"
#include "input2.h"
#include "logger2.h"
#include "module2.h"
#include "output2.h"
#include "plugin2.h"
