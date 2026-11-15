#pragma once
// Selects this build's embedded built-in MIDL records (docs/midl/midl-only.md),
// per MIDL display class exactly like generated_midl_manifest.h. The per-class
// headers under include/midl_gen/<class>/builtin_records.h are AUTO-GENERATED
// by tools/embed_builtin_records.py (`make embed-records`) and committed.
#if defined(YB_MIDL_CLASS_ROUND_360)
#include "midl_gen/round-360/builtin_records.h"
#elif defined(YB_MIDL_CLASS_LANDSCAPE_800X480)
#include "midl_gen/landscape-800x480/builtin_records.h"
#elif defined(YB_MIDL_CLASS_LANDSCAPE_1024X600)
#include "midl_gen/landscape-1024x600/builtin_records.h"
#elif defined(YB_MIDL_CLASS_SQUARE_480) || !defined(ESP_PLATFORM)
#include "midl_gen/square-480/builtin_records.h"
#else
#error "No MIDL display class for this env: add pre:tools/midl_class.py to its extra_scripts"
#endif
