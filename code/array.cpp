//NOTE (Aske): I don't really like this entire pseudo-array stuff
//But it's a workaround to make it more like what I'm used to from GC'ed languages.
//I'd much rather have language-level arrays and strong static polymorphism.
//But mostly I just suspect there's simpler solutions in this context, if pursued.
//I get that feeling any time I use a generallized solution for a specific use-case.

#define PushPolyArray(arena, arrayPtrName, bufferSize, type)\
PushStructBuffer(arena, array_##type, bufferSize);		\
arrayPtrName->base = (##type *)(arrayPtrName + 1);		\
arrayPtrName->size = bufferSize

struct array_s32
{
    s64 size;
    s64 count;
    s32 *base;
};

internal void Array32Add(array_s32 *array, s32 element)
{
    Assert(array->count < array->size);
    *(array->base + array->count++) = element;
}

internal s32 Array32Get(array_s32 *array, s64 index)
{
    Assert(index < array->count);
    s32 result = *(array->base + index);
    return result;
}

internal b32 Array32Contains(array_s32 *array, s32 value)
{
    s32* start = array->base;
    s32* end = start + array->count;
    for (s32 *current = start; current < end; current += 1)
    {
        if (*current == value) return true;
    }
    return false;
}

internal void Array32MergeSortAscending(array_s32 *array, memory_arena *arena)
{
    s64 count = array->count;
    if (count > 2)
    {
        //STUDY (Aske): Is SIMD possible in larger arrays?

        SaveArena(arena);
        //NOTE (Aske): We ping-pong passes between array and shadowArray
        array_s32 *shadowArray = PushPolyArray(arena, shadowArray, count, s32);
        shadowArray->count = count; //NOTE (Aske): Unnecessary. Perhaps it minimizes confusion if debugging?
 
        for (s32 clusterSize = 1, depth = 1; clusterSize <= (count - 1); clusterSize *= 2, depth++)
        {
            //NOTE (Aske): clustersRemaining includes stub clusters at the end
            s64 clustersRemaining = (count + clusterSize - 1) / clusterSize;
            for (; clustersRemaining >= 2; clustersRemaining -= 2)
            {
                //STUDY (Aske): This looks like candidates for branchless programming
                //TODO (Aske): This should start at the end and go in reverse order.
                //Test to see if this is the case.
                s64 rightOffset = ((clustersRemaining * clusterSize) - 1);
                s32 *rightIn = (depth & 1 ? array->base : shadowArray->base) + rightOffset;
                s32 *leftIn = rightIn - clusterSize;
                s32 *rightOut = (depth & 1 ? shadowArray->base : array->base) + rightOffset;
                s32 *leftOut = rightOut - clusterSize;
     
                s32 *rightInEnd = leftIn;
                s32 *leftInEnd;
                if (clustersRemaining > 2)
                {
                    leftInEnd = leftIn - clusterSize;
                }
                else
                {
                    leftInEnd = (depth & 1 ? array->base : shadowArray->base);
                }
                
                Assert(!"Not implemented");
                    /* TODO (Aske): the pair is always sorted, so for each element,
                     * check which one is highest and copy that one, to the out-array.
                     * Then advance the pointers of both the out-array and the copied one,
                     * and repeat until we hit the end of the cluster for one pointer.
                     * Then copy the remaining of the other one.
                     */
            }

            if (clustersRemaining) //== 1
            {
                //NOTE (Aske): We can never have a leftover cluster in interim passes.
                Assert((clusterSize * 2) <= (count - 1));

                //TODO (Aske): And idea for a (perhaps 1-10%) perf boost:
                //It might often be the case for larger arrays that the remaining cluster
                //will be identical for multiple passes, in which case it might be more efficient
                //to do a one-time calculation of how many passes this exact cluster will remain,
                //Then we could infer which array to keep the value in for the next pass,
                //to avoid copying unchanged values back and forth unnnecessarily.
                //E.g. array size 4793, which starts with two single-remainder clusters,
                //and then at clusterSize 1024 and 2048 we have a 697-remainder cluster sticking around.

                s32 *source;
                s32 *destination;
                if (depth & 1)
                {
                    source = array->base;
                    destination = shadowArray->base;
                }
                else
                {
                    source = shadowArray->base;
                    destination = array->base;
                }

                NaiveWiderCopy(clusterSize * sizeof(s32), source, destination);
            }
        }

        ZeroRestoreArena(arena);
    }
    else if (count == 2)
    {
        s32 *first = array->base;
        s32 *last = first + 1;
        if (*first > *last)
        {
            s32* swapTemp = first;
            first = last;
            last = swapTemp;
        }
    }

#if ASH_SLOW
    for (s64 i = 0; i < (count - 1); ++i)
    {
        s32 *current = array->base + i;
        s32 *next = current + 1;

        Assert(*current <= *next);
    }
#endif
}

internal b32 Array32SortedIndexOf(array_s32 *array, s32 value, s64 *result)
{
    s32 *left = array->base;
    s32 *right = array->base + (array->count - 1);
    while (left <= right)
    {
        s32 *mid = left + ((right - left) / 2);
        if (*mid == value)
        {
            *result = (mid - array->base);
            return true;
        }

        if (value < *mid) right = (mid - 1);
        else left = (mid + 1);
    }
    *result = 0;
    return false;
}

enum label_type
{
    Type_label_forward,
    Type_label_backward,
};

struct label_position
{
    s32 byteAddress;
    s32 poolOffset;
};

struct array_label_position
{
    s64 size;
    s64 count;
    label_position *base;
    label_type type;
};

internal void ArrayLabelPosAdd(array_label_position *array, s32 byteAddress, s32 stringPoolPos)
{
    Assert(array->count < array->size);
    label_position *labelPos = (array->base + array->count++);
    labelPos->byteAddress = byteAddress;
    labelPos->poolOffset = stringPoolPos;
}

internal b32 ArrayLabelPosFromFilebyteSorted(array_label_position *array, s32 fileByte, label_position *result)
{
    label_position *left = array->base;
    label_position *right = array->base + (array->count - 1);
    while (left <= right)
    {
        label_position *mid = left + ((right - left) / 2);
        if (mid->byteAddress == fileByte)
        {
            *result = *mid;
            return true;
        }

        if (fileByte < mid->byteAddress) right = (mid - 1);
        else left = (mid + 1);
    }

    *result = { 0, 0 };
    return false;
}

internal b32 ArrayLabelPosFromFilebyte(array_label_position *array, s32 fileByte, label_position *result)
{

    for (label_position *cursor = array->base; cursor < (array->base + array->count); ++cursor)
    {
        if (cursor->byteAddress == fileByte)
        {
            result = cursor;
            return true;
        }
    }

    *result = { 0 , 0 };
    return false;
}



struct bit_array
{
    s64 count;
    u64 *slots;
};

internal bit_array * MakeBitArray(memory_arena *arena, s64 count)
{
    s64 realCount = (count + 63) >> 6;
    bit_array *result = PushStructBuffer(arena, bit_array, realCount);
    result->slots = (u64 *)(result + 1);
    result->count = count;
    return result;
}

internal void BitArray_SetBit(bit_array *array, s64 i)
{
    Assert(i < array->count);
    array->slots[i >> 6] |= (1 << (i & 63));
}

internal b32 BitArray_IndexOfFirstUnsetBit(bit_array *array, s64 *result)
{
    //NOTE (Aske): The 72 quintillion limit is definitely likely to be exceeded, hehe
    Assert(((array->count) << 6) >= array->count);

    u64 slotCount = array->count >> 6; // /64
    for (int slotIndex = 0; slotIndex < slotCount; ++slotIndex)
    {
        u64 slot = array->slots[slotIndex];
        s64 baseBitIndex = slotIndex * 64;
        //NOTE (Aske): Don't check the padding
        s64 limit = Minimum(63, array->count - baseBitIndex - 1);

        u64 bit = 1;
        for (int i = 0; i <= limit; ++i)
        {
            if ((slot & bit) != 0)
            {
                *result = baseBitIndex + i;
                return true;
            }
            bit <<= 1;
        }
    }
    *result = 0;
    return false;
}