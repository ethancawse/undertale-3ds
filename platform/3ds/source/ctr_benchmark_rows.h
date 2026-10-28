// Translation-unit-local stdio seam. Existing formats and writer bodies stay put.
static FILE* benchRowsOpen(const char*, const char*);
static int benchRowsStat(const char*, struct stat*);
static int benchRowsPuts(const char*, FILE*);
static size_t benchRowsWrite(const void*, size_t, size_t, FILE*);
static int benchRowsPutc(int, FILE*);
static int benchRowsPrintf(FILE*, const char*, ...) __attribute__((format(printf, 2, 3)));
static int benchRowsClose(FILE*);
static void benchRowsBegin(void);
static void benchRowsSave(void);
static BenchScriptState benchRowsTick(BenchScript*, Runner*);
#define fopen benchRowsOpen
#define stat(path, result) benchRowsStat(path, result)
#ifdef __3DS__
// Preserve GCC's original fputs(literal) -> fwrite conversion and its old
// caller bytes. These assembler names affect only this translation unit.
extern int fputs(const char*, FILE*) __asm__("benchRowsPuts");
extern size_t fwrite(const void*, size_t, size_t, FILE*) __asm__("benchRowsWrite");
#else
#define fputs benchRowsPuts
#define fwrite benchRowsWrite
#endif
#define fputc benchRowsPutc
#define fprintf benchRowsPrintf
#define fclose benchRowsClose
