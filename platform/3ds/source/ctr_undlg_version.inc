/* Select the shared table's existing Steam row; no duplicate digest. */
#define DATA_VERSIONS_UT_STEAM_ONLY
#define DATA_VERSION(game, part, size, digest) static const char roleDigest[] UD_RO = digest;
#include "../data_versions.inc"
#undef DATA_VERSION
#undef DATA_VERSIONS_UT_STEAM_ONLY
