#ifndef MB_MOCHIJSNATIVE_ABI_H
#define MB_MOCHIJSNATIVE_ABI_H
/*
 * mochijsnative_abi.h -- the v0.4.0 JS-visible ABI of mochijsnative.
 * Generated from the C export table (mn_tab); keep in sync by regenerating.
 *
 * Handles: runtimes/contexts/values/jits are small integer ids into tables
 * inside the module; values are MBJSValue references owned by their slot.
 * Strings returned to JS are engine-owned copies. Numbers that stand for
 * pointers or float64 bit patterns travel as BigInt (uint64).
 */
#include <stdint.h>

#define MOCHIJSNATIVE_ABI_VERSION "0.4.0"
#define MOCHIJSNATIVE_EXPORT_COUNT 65

/* every JS export, in registration order */
static const char *const MOCHIJSNATIVE_EXPORTS[MOCHIJSNATIVE_EXPORT_COUNT] = {
    "version",                 /* 0 */
    "abi",                     /* 1 */
    "exportNames",             /* 2 */
    "exportSignature",         /* 3 */
    "hostBase",                /* 4 */
    "hostExport",              /* 5 */
    "engineVersion",           /* 6 */
    "bind",                    /* 7 */
    "call",                    /* 8 */
    "unbind",                  /* 9 */
    "lastError",               /* 10 */
    "runtimeNew",              /* 11 */
    "runtimeFree",             /* 12 */
    "runtimeLimits",           /* 13 */
    "contextNew",              /* 14 */
    "contextFree",             /* 15 */
    "contextStd",              /* 16 */
    "valueNew",                /* 17 */
    "valueFree",               /* 18 */
    "valueType",               /* 19 */
    "valueIsException",        /* 20 */
    "valueToNumber",           /* 21 */
    "valueToInt",              /* 22 */
    "valueToString",           /* 23 */
    "valueJson",               /* 24 */
    "eval",                    /* 25 */
    "evalModule",              /* 26 */
    "detectModule",            /* 27 */
    "getException",            /* 28 */
    "dumpError",               /* 29 */
    "await",                   /* 30 */
    "loop",                    /* 31 */
    "callValue",               /* 32 */
    "getGlobal",               /* 33 */
    "getProp",                 /* 34 */
    "setProp",                 /* 35 */
    "jsonParse",               /* 36 */
    "jitNew",                  /* 37 */
    "jitInfo",                 /* 38 */
    "jitStats",                /* 39 */
    "jitCompile",              /* 40 */
    "jitInvoke",               /* 41 */
    "jitCall",                 /* 42 */
    "jitCleanup",              /* 43 */
    "jitFree",                 /* 44 */
    "valueDup",                /* 45 */
    "valueToBool",             /* 46 */
    "valueEq",                 /* 47 */
    "valueIsError",            /* 48 */
    "valueIsFunction",         /* 49 */
    "newObject",               /* 50 */
    "newArray",                /* 51 */
    "propDefine",              /* 52 */
    "propHas",                 /* 53 */
    "propDelete",              /* 54 */
    "propKeys",                /* 55 */
    "protoGet",                /* 56 */
    "protoSet",                /* 57 */
    "throwError",              /* 58 */
    "exceptionHas",            /* 59 */
    "memoryUsage",             /* 60 */
    "bigintNew",               /* 61 */
    "stringify",               /* 62 */
    "handleStats",             /* 63 */
    "promiseNew",              /* 64 */
};

#endif /* MB_MOCHIJSNATIVE_ABI_H */
