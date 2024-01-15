typedef int8_t s8; //signed integers, 8-64bit
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint8_t u8; //unsigned integers, 8-64bit
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef s32 b32; //32-bit boolean (for alignment)
typedef s8 b8;   //8-bit boolean (for alignment)

typedef float f32;
typedef double f64;

typedef uintptr_t umm;
typedef intptr_t smm;

#define internal static //tells the linker this function won't be exported
#define global_variable static //no export (translation unit), but also auto-zero
#define local_persist static //dangerous, undefined ASM, lazy init, locks - should only be used in entry function

#if !defined(ASH_SLOW)
#define ASH_SLOW 1
#endif

#if !defined(ASH_INTERNAL)
#define ASH_INTERNAL 1
#endif

#if !defined(LABEL_PADDING)
#define LABEL_PADDING 1
#endif

#define Sint16Max 0x7FFF
#define Sint32Max 0x7FFFFFFF
#define Uint16Max 0xFFFF
#define Uint32Max 0xFFFFFFFF
#define Uint64Max 0xFFFFFFFFFFFFFFFF

#define Int32MaxDigits 10

#define ArrayCount(arr) (sizeof((arr)) / (sizeof((arr)[0])))
#define Kilobytes(number) ((number) * 1024ull)
#define Megabytes(number) (Kilobytes(number) * 1024ull)
#define Gigabytes(number) (Megabytes(number) * 1024ull)
#define Terabytes(number) (Gigabytes(number) * 1024ull)
#define Minimum(a, b) ((a) < (b) ? (a) : (b))
#define Maximum(a, b) ((a) > (b) ? (a) : (b))

#if ASH_SLOW
#define Assert(Expression) if(!(Expression)) {*(int*)0 = 0;}
#else
#define Assert(Expression)
#endif
#define InvalidCodePath Assert(!"InvalidCodePath")
#define InvalidDefaultCase default: {InvalidCodePath;} break
#define InvalidCase(Value) case Value: {InvalidCodePath;} break

inline u32 SafeTruncateUInt64(u64 value)
{
    Assert(value <= Uint32Max);
    u32 result = (u32)value;
    return result;
}

internal void* NaiveSlowCopy(size_t byteCount, void *sourceInit, void *destInit)
{
	u8 *source = (u8 *)sourceInit;
	u8 *destination = (u8 *)destInit;
	while (byteCount--) { *destination++ = *source++; }
    return(destination);
}

internal void SlowCopyAdvancing(size_t byteCount, void *sourceInit, u8 **destInit)
{
    u8 *source = (u8 *)sourceInit;
    u8 *destination = *destInit;
    while (byteCount--) { *destination++ = *source++; }
    *destInit = destination;
}

internal void* NaiveWiderCopy(size_t byteCount, void *sourceInit, void *destInit)
{
    size_t clusterCount = byteCount >> 3;
    u32 remainder = byteCount & 7;

    //TODO (Aske): Use SIMD
    u64 *wideSource = (u64 *)sourceInit;
    u64 *wideDest = (u64 *)destInit;
    if(clusterCount)
    {
        while(clusterCount--) { *wideDest++ = *wideSource++; }
    }

    u8 *source = (u8 *)wideSource;
    u8 *destination = (u8 *)wideDest;
    if (remainder)
    {
        while (remainder--) { *destination++ = *source++; }
    }

    return (destination);
}

struct memory_arena
{
    size_t size;
    u8 *base;
    size_t used;
    u32 _savePointCount;
    size_t _savePoints[8];
};

internal void ZeroSize(void *ptr, size_t size)
{
    u8 *byte = (u8 *)ptr;
    while(size--) { *byte++ = 0; }
}
#define ZeroArray(array) for (size_t i = 0; i < ArrayCount(array); ++i) array[i] = 0

internal void PopSize(memory_arena *arena, size_t size)
{
    Assert(arena->used >= size);
    arena->used -= size;
}
internal void ZeroPopSize(memory_arena *arena, size_t size)
{
    Assert(arena->used >= size);
    size_t oldMemoryIndex = arena->used;
    arena->used -= size;
    ZeroSize(arena->base + arena->used, oldMemoryIndex - arena->used);
}

internal void SaveArena(memory_arena *arena)
{
    Assert(arena->_savePointCount < ArrayCount(arena->_savePoints));
    arena->_savePoints[arena->_savePointCount++] = arena->used;
}

internal void RestoreArena(memory_arena *arena)
{
    Assert(arena->_savePointCount > 0);
    arena->used = arena->_savePoints[--arena->_savePointCount];
}

internal void ZeroRestoreArena(memory_arena *arena)
{
    Assert(arena->_savePointCount > 0);
    size_t oldMemoryIndex = arena->used;
    arena->used = arena->_savePoints[--arena->_savePointCount];
    ZeroSize(arena->base + arena->used, oldMemoryIndex - arena->used);
}

internal void * PushSize(memory_arena *arena, size_t size)
{
    //Last byte needs to be 0 for strings, so (newUsed <= size) is not allowed
    Assert((arena->used + size) < arena->size);
    u8 *result = arena->base + arena->used;
    arena->used += size;
    return (void *)result;
}

#define PushStruct(arena, type) (type *)PushSize((arena), sizeof(type))
//NOTE (Aske): bufferSize is in bytes
#define PushStructBuffer(arena, type, bufferSize) (type *)PushSize((arena), (bufferSize + sizeof(type)))
#define PushArray(arena, count, type) (type *)PushSize(arena, (count)*sizeof(type))