#include <stdarg.h>

struct string
{
	s64 count;
	union
	{
		char *data;
		u8 *base;
	};
};

struct string_buffer
{
	s64 capacity;
	union
	{
		string asString;
		struct
		{
			s64 count;
			union
			{
				char *data;
				u8 *base;
			};
		};
	};
};

//STUDY (Aske): I think ideally string should be a compile-time constant, not something you can update.
//Appending a pre-allocated buffer like this will likely be a source of bugs
inline void StringSlowAppendPreallocated(char c, string *destination)
{
	*(destination->base + destination->count) = c;
	destination->count++;
}

internal void StringSlowAppendPreallocated(size_t byteCount, char *sourceInit, string *destination)
{
	u8 *source = (u8 *)sourceInit;
	u8 *at = destination->base + destination->count;
	destination->count += byteCount;
	while (byteCount--)
	{
		*at++ = *source++;
	}
}

internal b32 StringLengthBounded(char *toEvaluate, s64 maxExpected, s64 *result)
{
	s64 tmpResult = -1; //allow 0 as a valid length
	while (++tmpResult <= maxExpected)
	{
		if (*(toEvaluate + tmpResult) == 0)
		{
			*result = tmpResult;
			return true;
		}
	}
	Assert(!"String is larger than expected");
	*result = 0;
	return false;
}

//NOTE (Aske): Will be erronerous if the string isn't null-terminated
internal s64 StringLength(char *toEvaluate)
{
	s64 result = 0;
	while (*toEvaluate++)
	{
		++result;
	}
	return result;
}

internal b32 StringLastIndexOf(char *str, size_t length, char c, size_t *result)
{
	char *current = str + (length);
	while (current-- != str)
	{
		if (*current == c)
		{
			*result = (current - str);
			return true;
		}
	}
	*result = 0;
	return false;
}



internal b32 StringLastIndexOf2(char *str, size_t length,
                                char c1, char c2, size_t *result)
{
	char *current = str + (length);
	while (current-- != str)
	{
		if (*current == c1 || *current == c2)
		{
			*result = (current - str);
			return true;
		}
	}
	*result = 0;
	return false;
}

internal s32 S32FromCharAdvancing(char **atInit)
{
	s32 result = 0;

	char *at = *atInit;
	while (	(*at >= '0') &&
			(*at <= '9'))
	{
		result *= 10;
		result += (*at - '0');
		++at;
	}

	*atInit = at;
	return result;
}

internal s32 S32FromChar(char *at)
{
	char *ignored = at;
	s32 result = S32FromCharAdvancing(&at);
	return result;
}

struct format_cursor
{
	size_t sizeRemaining;
	char *at;
};

inline void OutChar(format_cursor *dest, char value)
{
	if (dest->sizeRemaining)
	{
		--dest->sizeRemaining;
		*dest->at++ = value;
	}
}

inline u64 ReadVarArgUnsignedInteger(u32 length, va_list *argList)
{
	u64 result = 0;
	switch (length)
	{
		case 1: { result = va_arg(*argList, u8); } break;
		case 2: { result = va_arg(*argList, u16); } break;
		case 4: { result = va_arg(*argList, u32); } break;
		case 8: { result = va_arg(*argList, u64); } break;
		InvalidDefaultCase;
	}
	return result;
}


inline s64 ReadVarArgSignedInteger(u32 length, va_list *argList)
{
	s64 result = 0;
	switch (length)
	{
		case 1: { result = va_arg(*argList, s8); } break;
		case 2: { result = va_arg(*argList, s16); } break;
		case 4: { result = va_arg(*argList, s32); } break;
		case 8: { result = va_arg(*argList, s64); } break;
		InvalidDefaultCase;
	}
	return result;
}

inline f64 ReadVarArgFloat(u32 length, va_list *argList)
{
	u64 result = 0;
	switch (length)
	{
		case 4: { result = va_arg(*argList, f32); } break;
		case 8: { result = va_arg(*argList, f64); } break;
		InvalidDefaultCase;
	}
	return result;
}

internal void U64ToAscii(format_cursor *dest, u64 value, u32 numBase, char* digits)
{
	Assert(numBase != 0);

	char *start = dest->at;
	do
	{
		u64 digitIndex = value % numBase;
		OutChar(dest, digits[digitIndex]);
		value /= numBase;
	}
	while (value != 0);
	//NOTE (Aske): reverse the buffer
	char *end = dest->at;
	while (start < end)
	{ //swap boundary cursor values until they meet
		--end;
		char tempSwap = *end;
		*end = *start;
		*start = tempSwap;
		++start;
	}
}

internal void F64ToAscii(format_cursor *dest, u64 value, f32 numBase, char* digits, u32 precision)
{
	if (value < 0)
	{
		OutChar(dest, '-');
		value = -value;
	}
	u64 integerPart = (u64)value;
	value -= (f64)integerPart;
	U64ToAscii(dest, integerPart, numBase, digits);

	//NOTE (Aske): loss of precision this way.
	//TODO (Aske): Better solution:
	//https://web.archive.org/web/20160122041251id_/http://cseweb.ucsd.edu:80/~lerner/papers/fp-printing-popl16.pdf
	for (u32 precisionIndex = 0; precisionIndex < precision; ++precisionIndex)
	{
		value *= numBase;
		u32 integer = (u32)value;
		value -= (f32)integer;
		OutChar(dest, digits[integer]);
	}
}

//NOTE (Aske): Result doesn't include null ternimator.
//Currently has slow support for strings; crude support for floats, j, z and t; and doesn't support UTF16. Otherwise follows sprintf specs, except for the addition of the %S specifier for length-prefixed strings
internal umm FormatStringList(size_t destSize, char* destInit, char *format, va_list argList)
{
	format_cursor output = { destSize, destInit };
	if (output.sizeRemaining)
	{
		char *at = format;
		while (at[0])
		{
			if (*at == '%')
			{
				++at;
				
				//NOTE (Aske): Flags specifier
				b32 forceSign = false;
				b32 padWithZeroes = false;
				b32 leftJustify = false;
				b32 positiveSignIsSpace = false;
				b32 annotateIfNotZero = false; //NOTE (Aske): 0, 0x and 0X respectively, for hex/octals
				
				b32 isParsingFlags = true;
				while (isParsingFlags)
				{
					switch (*at)
					{
						case '+': {forceSign = true; } break;
						case '0': { padWithZeroes = true; } break;
						case '-': { leftJustify = true; } break;
						case ' ': { positiveSignIsSpace = true; } break;
						case '#': { annotateIfNotZero = true; } break;
						default: { isParsingFlags = false; } break;
					}

					if (isParsingFlags)
					{
						++at;
					}
				}

				// NOTE (Aske): Width specifier
				b32 isWidthSpecified = false;
				s32 specifiedWidth = 0;
				if (*at == '*')
				{
					isWidthSpecified = true;
					specifiedWidth = va_arg(argList, int);
					++at;
				}
				else if ((*at >= '0') && (*at <= '9'))
				{
					isWidthSpecified = true;
					specifiedWidth = S32FromCharAdvancing(&at);
				}

				//NOTE (Aske): Precision specifier
				b32 isPrecisionSpecified = false;
				s32 specifiedPrecision = 0;
				if (*at == '.')
				{
					++at;
					if (*at == '*')
					{
						isPrecisionSpecified = true;
						specifiedPrecision = va_arg(argList, int);
						++at;
					}
					else if ((*at >= '0') && (*at <= '9'))
					{
						isPrecisionSpecified = true;
						specifiedPrecision = S32FromCharAdvancing(&at);
					}
					else
					{
						isPrecisionSpecified = true;
						specifiedPrecision = 0;
					}
				}

				//NOTE (Aske): Length specifier
				u32 integerLength = 4;
				u32 floatLength = 8;
				u32 charLength = 1;
				if ((at[0] == 'h') && (at[1] == 'h'))
				{
					integerLength = 1;
					at += 2;
				}
				else if (at[0] == 'h')
				{
					integerLength = 2;
					at += 1;
				}
				else if (at[0] == 'l')
				{
					integerLength = 4;
					charLength = 2;
					at += 1;
				}
				else if ((at[0] == 'l') && (at[1] == 'l'))
				{
					integerLength = 8;
					at += 2;
				}
				else if (at[0] == 'j') //TODO (Aske): Handle "max supported"
				{
					integerLength = 8;
					at += 1;
				}
				else if (at[0] == 'z') //TODO (Aske): Handle size_t 
				{
					integerLength = 4;
					at += 1;
				}
				else if (at[0] == 't') //TODO (Aske): Handle ptrdiff_t
				{
					integerLength = 4;
					at += 1;
				}
				else if (at[0] == 'L')
				{ 
					floatLength = 8;
					at += 1;
				}

				//NOTE (Aske): Specifier character
				char decChars[] = "0123456789";
				char lowerHexChars[] = "0123456789abcdef";
				char upperHexChars[] = "0123456789ABCDEF";
				
				char prePassBuffer[64]; //NOTE (Aske): only defined for compile-time size
				char *processingBuffer = prePassBuffer;
				
				format_cursor processingCursor = { sizeof(prePassBuffer), processingBuffer };
				char *prefix = "";
				b32 isFloat = false;

				switch (*at)
				{
					case 'd':
					case 'i': //signed int
					{
						s64 value = ReadVarArgSignedInteger(integerLength, &argList);
						b32 wasNegative = value < 0;
						if (wasNegative)
						{
							value = -value;
						}
						U64ToAscii(&processingCursor, (u64)value, 10, decChars);

						if (wasNegative)
						{
							prefix = "-";
						}
						else if (forceSign)
						{
							Assert(!positiveSignIsSpace); //NOTE (Aske): Bad specifier, but doesn't give problems
							prefix = "+";
						}
						else if (positiveSignIsSpace)
						{
							prefix = " ";
						}
					} break;

					case 'u': //unsigned int
					{
						u64 value = ReadVarArgUnsignedInteger(integerLength, &argList);
						U64ToAscii(&processingCursor, value, 10, decChars);
					} break; 

					case 'o': //unsigned octal
					{
						u64 value = ReadVarArgUnsignedInteger(integerLength, &argList);
						U64ToAscii(&processingCursor, value, 8, decChars);
						if (annotateIfNotZero && value != 0)
						{
							prefix = "0";
						}
					} break;

					case 'x': //unsigned hexadecimal int
					{
						u64 value = ReadVarArgUnsignedInteger(integerLength, &argList);
						U64ToAscii(&processingCursor, value, 16, lowerHexChars);
						if (annotateIfNotZero && value != 0) {
							prefix = "0x";
						}
					} break;

					case 'X': //unsigned hexadecimal int (uppercase)
					{
						u64 value = ReadVarArgUnsignedInteger(integerLength, &argList);
						U64ToAscii(&processingCursor, value, 16, upperHexChars);
						if (annotateIfNotZero && value != 0)
						{
							prefix = "0X";
						}
					} break;

					case 'f': //float lowercase //TODO (Aske): Support NaN and Infinity
					case 'F': //float uppercase 
					case 'e': //scientific notation lowercase //TODO (Aske): Support e, E, g and G
					case 'E': //scientific notation uppercase
					case 'g': //shortest of %e and %f
					case 'G': //shortest of %E and %F
					{ //TODO (Aske): Support this
						f64 value = ReadVarArgFloat(floatLength, &argList);
						F64ToAscii(&processingCursor, value, 10, decChars, isPrecisionSpecified ? specifiedPrecision : 6);
						isFloat = true;
					} break;

					case 'a': //hexadecimal float lowercase //TODO (Aske): Support NaN and Infinity
					{
						f64 value = ReadVarArgFloat(floatLength, &argList);
						F64ToAscii(&processingCursor, value, 16, lowerHexChars, isPrecisionSpecified ? specifiedPrecision : 6);
						isFloat = true;
					} break;

					case 'A': //hexadecimal float uppercase //TODO (Aske): Support NaN and Infinity
					{
						f64 value = ReadVarArgFloat(floatLength, &argList);
						F64ToAscii(&processingCursor, value, 16, upperHexChars, isPrecisionSpecified ? specifiedPrecision : 6);
						isFloat = true;
					} break;

					case 'c': //character
					{
						//TODO (Aske): Handle w_char when charLength == 2
						int value = va_arg(argList, int);
						OutChar(&processingCursor, (char)value);
					} break;

					case 's': //C-string
					{
						char *strArg = va_arg(argList, char *);

						//NOTE (Aske): This is slow, but it's fine for debug
						processingBuffer = strArg;
						if (isPrecisionSpecified)
						{
							processingCursor.sizeRemaining = 0;
							for (char *scan = strArg;
								*scan && (processingCursor.sizeRemaining < specifiedPrecision);
								++scan)
							{
								++processingCursor.sizeRemaining;
							}
						}
						else
						{
							processingCursor.sizeRemaining = StringLength(strArg);
						}
						processingCursor.at = strArg + processingCursor.sizeRemaining;
					} break;

					case 'S': //custom string
					{
						string strArg = va_arg(argList, string);

						processingBuffer = strArg.data;
						processingCursor.sizeRemaining = strArg.count;
						if (isPrecisionSpecified && (processingCursor.sizeRemaining > specifiedPrecision))
						{
							processingCursor.sizeRemaining = specifiedPrecision;
						}
						processingCursor.at = processingBuffer + processingCursor.sizeRemaining;
					} break;

					case 'p': //pointer address
					{
						void *value = va_arg(argList, void *);
						U64ToAscii(&processingCursor, *(umm *)value, 16, lowerHexChars);
					} break;

					case 'n': //prints nothing and outputs current character count to caller
					{ 		 //e.g. for tab spacing or formatting blocks
							 //("%s stuff %nbold text%n stuff %s", ..., &startBold, &endBold, ...)
						int *currentCharIndex = va_arg(argList, int *);
						*currentCharIndex = (int)(output.at - destInit);
					} break;

					case '%': //literal '%'
					{
						OutChar(&output, '%');
					} break;

					default:
					{
						Assert(!"Unrecognized format specifier");
					} break;
				}

				//NOTE (Aske): Write processed numbers to output
				if (processingCursor.at - processingBuffer) //processingCursor has contents
				{
					smm precisionRemaining = specifiedPrecision;
					//NOTE (Aske): floats handled precision in previous preprocess step
					if (isFloat || !isPrecisionSpecified)
					{
						precisionRemaining = (processingCursor.at - processingBuffer);
					}

					smm prefixLength = StringLength(prefix);
					smm widthRemaining = specifiedWidth;
					smm minimumWidth = precisionRemaining + prefixLength;
					if (widthRemaining < minimumWidth)
					{
						widthRemaining = minimumWidth;
					}

					if (padWithZeroes)
					{
						Assert(!leftJustify); //NOTE (Aske): Probably erroneous request - doesn't give problems
						leftJustify = false;
					}

					//NOTE (Aske): Pad with width, right justify
					if (!leftJustify)
					{
						while (widthRemaining > (precisionRemaining + prefixLength))
						{
							OutChar(&output, padWithZeroes ? '0' : ' ');
							--widthRemaining;
						}
					}

					for (char *prefixAt = prefix;
						*prefixAt && widthRemaining;
						++prefixAt)
					{
						OutChar(&output, *prefixAt);
						--widthRemaining;
					}

					if (precisionRemaining > widthRemaining)
					{
						precisionRemaining = widthRemaining;
					}
					//NOTE (Aske): Pad with precision
					while (precisionRemaining > (processingCursor.at - processingBuffer))
					{
						OutChar(&output, '0');
						--precisionRemaining;
						--widthRemaining;
					}
					while (precisionRemaining && (processingCursor.at != processingBuffer))
					{
						OutChar(&output, *processingBuffer++);
						--precisionRemaining;
						--widthRemaining;
					}
					//NOTE (Aske): Pad with width, left justify
					if (leftJustify)
					{
						while (widthRemaining)
						{
							OutChar(&output, ' ');
							--widthRemaining;
						}
					}
				}
				
				//NOTE (Aske): Make sure we don't advance if we've reached the end of format
				if (*at)
				{
					++at;
				}
			}
			else
			{
				OutChar(&output, *at++);
			}
		}
		
		//Ensure null-terminator
		if (output.sizeRemaining)
		{
			output.at[0] = 0;
		}
		else
		{
			//NOTE (Aske): Make sure that the size consistently doesn't include null terminator
			--*output.at = 0;
		}
	}
	
	umm outputSizeExclTerminator = output.at - destInit;
	return outputSizeExclTerminator;
}

internal size_t FormatString(size_t destSize, char* dest, char *format, ...)
{
	va_list argList;

	va_start(argList, format);
	size_t result = FormatStringList(destSize, dest, format, argList);
	va_end(argList);
	return result;
}

internal size_t FormatStringBufferFromBase(string_buffer *buffer, char *format, ...)
{
	va_list argList;

	va_start(argList, format);
	size_t result = FormatStringList(buffer->capacity, buffer->data, format, argList);
	va_end(argList);
	
	buffer->count = result;
	return result;
}

internal size_t FormatStringBufferFromOffset(string_buffer *buffer, s64 offset, char *format, ...)
{
	va_list argList;

	va_start(argList, format);
	size_t result = FormatStringList(buffer->capacity - offset, buffer->data + offset, format, argList);
	va_end(argList);
	
	buffer->count += result;
	return result;
}