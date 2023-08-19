#include <Windows.h>
#include <stdint.h>
#include <string>
#include <string.h>
typedef int8_t int8;
typedef int16_t int16;
typedef int32_t int32;
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef int32 bool32;

#define internal static
#define local_persist static 
#define global_variable static

#if !defined(ASH_SLOW)
#define ASH_SLOW 1
#endif

#define Uint32Max 0xFFFFFFFF

#if ASH_SLOW
#define Assert(Expression) if(!(Expression)) {*(int*)0 = 0;}
#else
#define Assert(Expression)
#endif
#define InvalidCodePath Assert(!"InvalidCodePath")
#define InvalidDefaultCase default: {InvalidCodePath;} break
#define InvalidCase(Value) case Value: {InvalidCodePath;} break

inline uint32
SafeTruncateUInt64(uint64 Value) {
	Assert(Value <= Uint32Max);
	uint32 Result = (uint32)Value;
	return(Result);
}

#pragma region File I/O
internal void
FreeFileMemory(void* Memory) {
	if (Memory) {
		VirtualFree(Memory, 0, MEM_RELEASE);
	}
}

struct debug_read_file_result
{
	uint32 ContentsSize;
	void* Contents;
};

internal debug_read_file_result
ReadEntireFile(char* Filename) {
	debug_read_file_result Result = {};

	HANDLE FileHandle = CreateFileA(Filename, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
	if (FileHandle != INVALID_HANDLE_VALUE) {
		LARGE_INTEGER FileSize;
		if (GetFileSizeEx(FileHandle, &FileSize)) {
			uint32 FileSize32 = SafeTruncateUInt64(FileSize.QuadPart);
			Result.Contents = VirtualAlloc(0, FileSize32, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
			if (Result.Contents) {
				DWORD BytesRead;
				if (ReadFile(FileHandle, Result.Contents, FileSize32, &BytesRead, 0) && (FileSize32 == BytesRead)) {
					//NOTE(aske): File read successfully
					Result.ContentsSize = FileSize32;
				}
				else {
					FreeFileMemory(Result.Contents);
					Result.Contents = 0;
				}
			}
		}
		CloseHandle(FileHandle);
	}
	return Result;
}
internal bool32
WriteEntireFile(char* Filename, uint32 MemorySize, void* Memory) {
	bool32 Result = false;

	HANDLE FileHandle = CreateFileA(Filename, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
	if (FileHandle != INVALID_HANDLE_VALUE) {
		DWORD BytesWritten;
		if (WriteFile(FileHandle, Memory, MemorySize, &BytesWritten, 0)) {
			//NOTE (aske): File read successfully
			Result = (BytesWritten == MemorySize);
		}
		CloseHandle(FileHandle);
	}

	return Result;
}
#pragma endregion

enum DisplacementMode : uint8 { //*ptr + displacementAmount
	Memory_None = 0,
	Memory_8bit = 1,
	Memory_16bit = 2,
	Register_None = 3,
};
enum Register8 : uint8 {
	AccumulatorLow, //Byte Multiply, Byte Divide, Byte I/O, Translate, Decimal Arithmetic
	CountLow, //Variable Shift and Rotate
	DataLow,
	BaseLow,
	AccumulatorHigh, //Byte Multiply, Byte Divide
	CountHigh,
	DataHigh,
	BaseHigh
};
enum Register16 : uint8 {
	AccumulatorXFull, //Word Multiply, Word Divide, Word I/O
	CountXFull, //String Operations, Loops
	DataXFull, //World Multiply, Word Divide, Indirect I/O
	BaseXFull, //Translate
	StackPointer, //Stack Operations
	BasePointer, 
	SourceIndex, //String Operations
	DestinationIndex, //String Operations
};

struct RegisterEncoding {
	union {
		uint8 value;
		Register8 byteRegister;
		Register16 wordRegister;
	};
};

char* NarrowRegisters[8] = { "al", "cl", "dl", "bl", "ah", "ch", "dh", "bh" };
char* WideRegisters[8] = { "ax", "cx", "dx", "bx", "sp", "bp", "si", "di" };
char* EffectiveAddressCalculations[8]{ "bx + si", "bx + di", "bp + si", "bp + di", "si", "di", "bp", "bx", };

internal void
Disassemble8086(char* binaryFileName, char* outputFileName) {
	debug_read_file_result file = ReadEntireFile(binaryFileName);
	void* entireBinary = file.Contents;
	uint32 binaryLengthInBytes = file.ContentsSize;
	uint32 outputLength = 8; //"bits 16\n"
	std::string output = "bits 16\n";
	uint8* byte1Cursor = (uint8*)entireBinary;
	uint8* byte2Cursor = byte1Cursor + 1;

	uint32 cursorIndex = 0;
	uint32 numBytesUntilNextRegister;
	while (cursorIndex < binaryLengthInBytes)
	{
		numBytesUntilNextRegister = 2;

		if ((*byte1Cursor & 0374) == 0210) //MOV1: Register/memory to/from register
		{
			output += "\nmov ";
			outputLength += 5;
			bool is16Bit = (*byte1Cursor & 01); //!8bit
			bool isREGDestination = (*byte1Cursor & 02); //REG either specifies Source or Destination

			RegisterEncoding registryEncoding = { (uint8)((*byte2Cursor & 070) >> 3) };
			DisplacementMode displacementMode = (DisplacementMode)((uint8)((*byte2Cursor & 0300) >> 6));
			uint8 rmEncodingValue = (uint8)(*byte2Cursor & 07);
			if (displacementMode == Register_None) //Register-to-register no displacement
			{
				RegisterEncoding secondRegistryEncoding = { rmEncodingValue };
				std::string firstOperand;
				std::string secondOperand;

				if (is16Bit)
				{
					firstOperand = WideRegisters[registryEncoding.value];
					secondOperand = WideRegisters[secondRegistryEncoding.value];
				}
				else { //8bit
					firstOperand = NarrowRegisters[registryEncoding.value];
					secondOperand = NarrowRegisters[secondRegistryEncoding.value];
				}
				output += isREGDestination ? firstOperand + ", " + secondOperand : secondOperand + ", " + firstOperand;
				outputLength += 6;
			}
			else if (displacementMode == Memory_None && rmEncodingValue == 06) { //DIRECT ADDRESSESS + 16 bit displacement
				numBytesUntilNextRegister += 2;
				char* firstOperand = is16Bit ? WideRegisters[registryEncoding.value] : NarrowRegisters[registryEncoding.value];

				uint16 displacementValue = *(uint16*)(byte2Cursor + 1);
				char displacementBuffer[5]; //65536
				sprintf(displacementBuffer, "%d", displacementValue);
				char addressBuffer[8] = "["; //[65536]
				strcat(addressBuffer, displacementBuffer);
				strcat(addressBuffer, "]");
				output += isREGDestination ? std::string(firstOperand) + ", " + addressBuffer : std::string(addressBuffer) + ", " + firstOperand;
				outputLength += strlen(firstOperand) + 2 + strlen(addressBuffer);
			}
			else { //Memory mode 0, 8-bit or 16-bit displacement, uniform cases
				char* firstOperand = is16Bit ? WideRegisters[registryEncoding.value] : NarrowRegisters[registryEncoding.value];

				char addressBuffer[18] = "["; //[bx + si + 65536]

				char* effectiveAddressCalculation = EffectiveAddressCalculations[rmEncodingValue];
				strcat(addressBuffer, effectiveAddressCalculation);

				switch (displacementMode)
				{
					case Memory_None: break;
					case Memory_8bit:
					{
						numBytesUntilNextRegister = 3;
						uint8* byte3Cursor = (byte2Cursor + 1);
						if (*byte3Cursor != 0)
						{
							char displacementBuffer[4]; //255 + null-terminator
							if (is16Bit) //Wide means signed
							{
								int8 signedDisplacementValue = *((int8*)byte3Cursor);
								uint8 displacementValue;
								if (signedDisplacementValue >= 0)
								{
									strcat(addressBuffer, " + ");
									displacementValue = (uint8)signedDisplacementValue;
								}
								else {
									strcat(addressBuffer, " - ");
									displacementValue = (uint8)-signedDisplacementValue;
								}
								sprintf(displacementBuffer, "%d", displacementValue);
							}
							else {
								strcat(addressBuffer, " + ");
								sprintf(displacementBuffer, "%d", *byte3Cursor);
							}
							strcat(addressBuffer, displacementBuffer);
						}
					} break;
					case Memory_16bit:
					{
						numBytesUntilNextRegister = 4;
						uint16* byte34Cursor = (uint16*)(byte2Cursor + 1);
						char displacementBuffer[6]; //65535 + null-terminator
						if (is16Bit) //Wide means signed
						{
							int16 signedDisplacementValue = *((int16*)byte34Cursor);
							uint16 displacementValue;
							if (signedDisplacementValue >= 0)
							{
								strcat(addressBuffer, " + ");
								displacementValue = (uint16)signedDisplacementValue;
							}
							else {
								strcat(addressBuffer, " - ");
								displacementValue = (uint16)-signedDisplacementValue;
							}
							sprintf(displacementBuffer, "%d", displacementValue);
						}
						else {
							strcat(addressBuffer, " + ");
							sprintf(displacementBuffer, "%d", *byte34Cursor);
						}
						strcat(addressBuffer, displacementBuffer);
					} break;
					InvalidDefaultCase;
				}

				strcat(addressBuffer, "]");

				output += isREGDestination ? std::string(firstOperand) + ", " + addressBuffer : std::string(addressBuffer) + ", " + firstOperand;
				outputLength += strlen(addressBuffer) + 2 + strlen(firstOperand); //", " + operand
			}
		}
		else if ((*byte1Cursor & 0376) == 0306) { //MOV immediate to register/memory {
			output += "\nmov ";
			outputLength += 5;
			bool is16Bit = (*byte1Cursor & 01); //!8bit
			DisplacementMode displacementMode = (DisplacementMode)((uint8)((*byte2Cursor & 0300) >> 6));
			Assert((*byte2Cursor & 0160) == 0); //Empty REG is required
			uint8 rmEncodingValue = (uint8)(*byte2Cursor & 07);

			if (displacementMode == Register_None) {
				Assert("Not implemented");
			}
			else if (displacementMode == Memory_None && rmEncodingValue == 06) { //DIRECT ADDRESSESS + 16 bit displacement
				Assert("Not implemented");
			}
			else { //Memory mode 0, 8-bit or 16-bit displacement, uniform cases
				uint8 dataByteOffsetByDisplacementBytes = 0;

				//------address-------
				char addressBuffer[18] = "["; //[bx + si + 65536]

				char* effectiveAddressCalculation = EffectiveAddressCalculations[rmEncodingValue];
				strcat(addressBuffer, effectiveAddressCalculation);

				switch (displacementMode)
				{
					case Memory_None: break;
					case Memory_8bit:
					{
						dataByteOffsetByDisplacementBytes = 1;
						uint8* byte3Cursor = (byte2Cursor + 1);
						if (*byte3Cursor != 0)
						{
							char displacementBuffer[4]; //255 + null-terminator
							if (is16Bit) //Wide means signed
							{
								int8 signedDisplacementValue = *((int8*)byte3Cursor);
								uint8 displacementValue;
								if (signedDisplacementValue >= 0)
								{
									strcat(addressBuffer, " + ");
									displacementValue = (uint8)signedDisplacementValue;
								}
								else {
									strcat(addressBuffer, " - ");
									displacementValue = (uint8)-signedDisplacementValue;
								}
								sprintf(displacementBuffer, "%d", displacementValue);
							}
							else {
								strcat(addressBuffer, " + ");
								sprintf(displacementBuffer, "%d", *byte3Cursor);
							}
							strcat(addressBuffer, displacementBuffer);
						}
					} break;
					case Memory_16bit:
					{
						dataByteOffsetByDisplacementBytes = 2;
						uint16* byte34Cursor = (uint16*)(byte2Cursor + 1);
						char displacementBuffer[6]; //65535 + null-terminator
						if (is16Bit) //Wide means signed
						{
							int16 signedDisplacementValue = *((int16*)byte34Cursor);
							uint16 displacementValue;
							if (signedDisplacementValue >= 0)
							{
								strcat(addressBuffer, " + ");
								displacementValue = (uint16)signedDisplacementValue;
							}
							else {
								strcat(addressBuffer, " - ");
								displacementValue = (uint16)-signedDisplacementValue;
							}
							sprintf(displacementBuffer, "%d", displacementValue);
						}
						else {
							strcat(addressBuffer, " + ");
							sprintf(displacementBuffer, "%d", *byte34Cursor);
						}
						strcat(addressBuffer, displacementBuffer);
					} break;
					InvalidDefaultCase;
				}
				strcat(addressBuffer, "]");

				//---------immediate data----------
				char dataBuffer[12]; //word -32768 //TODO (aske): Can this be signed?
				if (is16Bit) {
					strcpy(dataBuffer,"word ");
					char valueBuffer[6];
					uint16 dataValue = *(uint16*)(byte2Cursor + 1 + dataByteOffsetByDisplacementBytes);
					sprintf(valueBuffer, "%d", dataValue);
					strcat(dataBuffer, valueBuffer);
					numBytesUntilNextRegister += dataByteOffsetByDisplacementBytes + 2;
				}
				else {
					strcpy(dataBuffer, "byte ");
					char valueBuffer[4];
					uint8 dataValue = *(uint8*)(byte2Cursor + 1 + dataByteOffsetByDisplacementBytes);
					sprintf(valueBuffer, "%d", dataValue);
					strcat(dataBuffer, valueBuffer);
					numBytesUntilNextRegister += dataByteOffsetByDisplacementBytes + 1;
				}

				output += std::string(addressBuffer) + ", " + std::string(dataBuffer);
				outputLength += strlen(addressBuffer) + 2 + strlen(dataBuffer);
			}
		}
		else if ((*byte1Cursor & 0360) == 0260) { //MOV immediate to register
			output += "\nmov ";
			outputLength += 5;
			bool is16Bit = (*byte1Cursor & 010); //!8bit
			RegisterEncoding registryEncoding = { (uint8)((*byte1Cursor & 07)) };
			
			if (is16Bit) {
				numBytesUntilNextRegister = 3;

				uint16* byte23Cursor = (uint16*)byte2Cursor;
				char numberBuffer[6];
				sprintf(numberBuffer, "%d", *byte23Cursor);
				output += std::string(WideRegisters[registryEncoding.value]) + ", " + numberBuffer;
				outputLength += 4 + strlen(numberBuffer);
			}
			else { //8-bit
				char numberBuffer[4];
				sprintf(numberBuffer, "%d", *byte2Cursor);
				output += std::string(NarrowRegisters[registryEncoding.value]) + ", " + numberBuffer;
				outputLength += 4 + strlen(numberBuffer);
			}
		}
		else if ((*byte1Cursor & 0376) == 0240) { //Memory to accumulator
			output += "\nmov ax, [";
			outputLength += 10;
			bool isSignedDisplacement = (*byte1Cursor & 010); //!8bit
			numBytesUntilNextRegister = 3;
			if (isSignedDisplacement)
			{
				Assert("Not implemented");
			}
			else { //8-bit
				uint16 displacementValue = *(uint16*)byte2Cursor;
				char numberBuffer[6];
				sprintf(numberBuffer, "%d", displacementValue);
				output += std::string(numberBuffer);
				outputLength += strlen(numberBuffer);
			}
			output += "]";
			outputLength += 1;
		}
		else if ((*byte1Cursor & 0376) == 0242) { //Accumulator to memory
			output += "\nmov [";
			outputLength += 6;
			bool isSignedDisplacement = (*byte1Cursor & 010); //!8bit
			numBytesUntilNextRegister = 3;
			if (isSignedDisplacement)
			{
				Assert("Not implemented");
			}
			else { //8-bit
				uint16 displacementValue = *(uint16*)byte2Cursor;
				char numberBuffer[6];
				sprintf(numberBuffer, "%d", displacementValue);
				output += std::string(numberBuffer);
				outputLength += strlen(numberBuffer);
			}
			output += "], ax";
			outputLength += 5;
		}
		else { //different instruction
			Assert("Not implemented"); break;
		}

		byte1Cursor += numBytesUntilNextRegister;
		byte2Cursor += numBytesUntilNextRegister;
		cursorIndex += numBytesUntilNextRegister;
	}
	WriteEntireFile(outputFileName, outputLength, const_cast<void*>(static_cast<const void*>(output.c_str())));
}

int main(int argc, char* argv[]) {
	char* inputFileName = argc > 1 ? argv[1] : "..\\data\\listing_0040_challenge_movs";
	char* outputFileName = argc > 2 ? argv[2] : "..\\data\\test3.asm";
	Disassemble8086(inputFileName, outputFileName);
	return 0;
}