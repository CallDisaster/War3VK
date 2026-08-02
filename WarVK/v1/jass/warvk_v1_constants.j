// WarVK JAPI v1 public constants.
// Include this file before warvk_v1.j when using raw JASS.
// All values and queries are local-visual only and must never drive
// multiplayer gameplay branches, orders, RNG seeds, or synchronized state.

globals
    constant integer WARVK_PROTOCOL_VERSION = 1

    constant integer WARVK_FEATURE_SUN = 1
    constant integer WARVK_FEATURE_CSM = 2
    constant integer WARVK_FEATURE_POINT_LIGHT = 4
    constant integer WARVK_FEATURE_VOLUMETRIC = 8
    constant integer WARVK_FEATURE_OUTLINE = 16
    constant integer WARVK_FEATURE_BLOOM = 32
    constant integer WARVK_FEATURE_POSTFX = 64
    constant integer WARVK_FEATURE_AA = 128
    constant integer WARVK_FEATURE_DAY_NIGHT = 256
    constant integer WARVK_FEATURE_LIGHTNING = 512
    constant integer WARVK_FEATURE_MANAGED_OBJECT = 1024
    constant integer WARVK_FEATURE_TIME = 2048
    constant integer WARVK_FEATURE_STATS = 4096

    constant integer WARVK_OBJECT_NONE = 0
    constant integer WARVK_OBJECT_POINT_LIGHT = 1
    constant integer WARVK_OBJECT_LIGHTNING = 2

    constant integer WARVK_ERROR_NONE = 0
    constant integer WARVK_ERROR_PAYLOAD_TOO_LONG = 1
    constant integer WARVK_ERROR_NON_ASCII = 2
    constant integer WARVK_ERROR_CONTROL_CHARACTER = 3
    constant integer WARVK_ERROR_EMPTY_TOKEN = 4
    constant integer WARVK_ERROR_TOO_MANY_ARGUMENTS = 5
    constant integer WARVK_ERROR_UNSUPPORTED_VERSION = 6
    constant integer WARVK_ERROR_MISSING_COMMAND = 7
    constant integer WARVK_ERROR_UNKNOWN_COMMAND = 8
    constant integer WARVK_ERROR_CARRIER_MISMATCH = 9
    constant integer WARVK_ERROR_ARGUMENT_COUNT = 10
    constant integer WARVK_ERROR_INVALID_INTEGER = 11
    constant integer WARVK_ERROR_INTEGER_OVERFLOW = 12
    constant integer WARVK_ERROR_INVALID_BOOLEAN = 13
    constant integer WARVK_ERROR_INVALID_ID = 14
    constant integer WARVK_ERROR_INVALID_REAL = 15
    constant integer WARVK_ERROR_REAL_OUT_OF_RANGE = 16
    constant integer WARVK_ERROR_BACKEND_UNAVAILABLE = 17
    constant integer WARVK_ERROR_UNSUPPORTED_FEATURE = 18
    constant integer WARVK_ERROR_BACKEND_REJECTED = 19
    constant integer WARVK_ERROR_INTERNAL = 20
    constant integer WARVK_ERROR_BACKEND_CONTRACT = 21
    constant integer WARVK_ERROR_INVALID_ARGUMENT_TYPE = 22
endglobals
