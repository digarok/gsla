//
// C++ Encoder/Decoder
// For GSLA, GS Lzb Animation File Format
// 
//
// Care is taken in the encoder, to make sure the 65816 does not have to cross
// bank boundaries during any copy.  This is so we can use the MVN instruction,
// and so we can reduce the number of bank checks in the code.  We will have an
// opcode, that says �source data bank has changed�
// 
// The file will be laid out such that you load the file in at a 64K memory
// boundary
//
// Goals include a good balance between file size, and playback performance
// (since one often makes a trade off with the other).
//
// The file is defined as a byte stream, loaded on a 64K Bank Boundary
//
//
// file-offset:   the thing that is at the offset
//
// Header of the File is 20 bytes as follows
//
//File Offset			Data	Commentary
//------------------------------------------------------------------
//0				0x47	; �G�  Graphics
//1				0x53	; �S� 
//2             0x4C    ; �L�  LZB
//3				0x41    ; �A�  Animation
//
// File Length, is the total length of the file
//4				FileLengthLow    ; Low byte, 32-bit file length
//5				LengthLowHigh    ; High byte of low word
//6				LengthHighLow    ; Low byte, of high word
//7				LengthHighHigh   ; High Byte, of high word
//
//  16 bit word with version #
//8				VL   ;  Version # of the file format, currently only version 0 exists
//9				VH   ; Version High Byte
//				     ; %RVVV_VVVV_VVVV_VVVV
//				     ; V is a version #, 0 for now
//					 ; R is the MSB,  R = 0 no ring frame
//					 ; R = 1, there is a ring frame
//					 ; A Ring Frame is a frame that will delta from the last
//                   ; frame of the animation, back to the first, for smoother
//                   ; playback looping ,  If a ring frame exists, it�s also
// 					 ; included in the frame count
//
// next is a word, width in bytes (likely 160 for now)
//0xA				WL      ; Display Width in bytes low byte
//0xB				WH      ; Display Width in bytes high byte
//
// next is a word, height (likely 200 for now)
//0xC				HL	    ; Display Height in bytes, low byte
//0xD				HH      ; Display Height in bytes, high byte
// 2 bytes, Frame Size in Bytes, since a �Frame� may contain more than just the
// width * height, worth of pixels, for now this is $8000, or 32768
//0xE				FBL     ; Frame Buffer Length Low
//0xF				FBH     ; Frame Buffer Length High
//
// 4 byte, 32-bit, Frame Count (includes total frame count, so if there is a ring frame, this is included in the total)
//0x10	FrameCountLow
//0x11	FrameCountLowHigh
//0x12	FrameCountHighLow
//0x13	FrameCountHigh
//
//
// After this comes AIFF style chunks of data,  basically a 4 byte chunk name,
// followed by a 4 byte length (inclusive of the chunk size).   The idea is that
// you can skip chunks you don�t understand.
//
//File Offset:
//0x14				First Chunk  (followed by more Chunks, until end of file)
//
//Chunk Definitions
//Name:  �INIT�   -  Initial Frame Chunk, this is the data used to first initialize the playback buffer
//0:	0x49	�I�
//1: 	0x4E	�N�
//2: 	0x49	�I�
//3: 	0x54	�T�
// 32 bit long, length, little endian, including the 8 byte header
//4:	length low low
//5:	length low high
//6:	length high low
//7:	length high high
//
//8: �.   This is a single frame of data, that decodes/decompresses into frame
//        sized bytes (right now 0x8000)
// This data stream includes, an end of animation opcode, so that the normal
// animation decompressor, can be called on this data, and it will emit the
// initial frame onto the screen
//
//Name: �ANIM� - Frames
//0:	0x41 �A�
//1:	0x4E �N�
//2:	0x49 �I�
//3:	0x4D �M�
// 32 bit long, length, little endian, including chunk header
//4:	length low low
//5:	length low high
//6:	length high low
//7:	length high high
//
// This is followed by the frames, with the intention of decompressing them at
// 60FPS, which is why no play speed is included, if you need a play-rate
// slower than this, blank frame�s should be inserted into the animation data
//
// Every attempt is made to delta encode the image,  meaning we just encode
// information about what changed each frame.   We attempt to make the size
// efficient by supporting dictionary copies (where the dictionary is made up
// of existing pixels in the frame buffer).
//
//Command Word, encoded low-high, what the bits mean:
//
// xxx_xxxx_xxxx_xxx is the number of bytes 1-16384 to follow (0 == 1 byte)
//
//%0xxx_xxxx_xxxx_xxx0 - Copy Bytes - straight copy bytes
//%1xxx_xxxx_xxxx_xxx1 - Skip Bytes - skip bytes / move the cursor
//%1xxx_xxxx_xxxx_xxx0 - Dictionary Copy Bytes from  frame buffer to frame buffer
//
//%0000_0000_0000_0001- Source Skip -> Source pointer skips to next bank of data
//%0000_0000_0000_0011- End of Frame - end of frame
//%0000_0000_0000_0111- End of Animation / End of File / no more frames
//
// other remaining codes, are reserved for future expansion

#include "gsla_file.h"
#include <stdio.h>

// If these structs are the wrong size, there's an issue with type sizes, and
// your compiler
static_assert(sizeof(GSLA_Header)==20, "GSLA_Header is supposed to be 20 bytes");
static_assert(sizeof(GSLA_INIT)==8,    "GSLA_INIT is supposed to be 8 bytes");
static_assert(sizeof(GSLA_ANIM)==8,    "GSLA_ANIM is supposed to be 8 bytes");
static_assert(sizeof(GSLA_CHUNK)==8,   "GSLA_CHUNK is supposed to be 8 bytes");

//------------------------------------------------------------------------------
// Load in a FanFile constructor
//
GSLAFile::GSLAFile(const char *pFilePath)
	: m_widthPixels(320)
	, m_heightPixels(200)
{
	LoadFromFile(pFilePath);
}
//------------------------------------------------------------------------------

GSLAFile::~GSLAFile()
{
	// Free Up the memory
	for (int idx = 0; idx < m_pC1PixelMaps.size(); ++idx)
	{
		delete[] m_pC1PixelMaps[idx];
		m_pC1PixelMaps[ idx ] = nullptr;
	}
}

//------------------------------------------------------------------------------

void GSLAFile::LoadFromFile(const char* pFilePath)
{
	// Free Up the memory
	for (int idx = 0; idx < m_pC1PixelMaps.size(); ++idx)
	{
		delete[] m_pC1PixelMaps[idx];
		m_pC1PixelMaps[ idx ] = nullptr;
	}

	m_pC1PixelMaps.clear();
	//--------------------------------------------------------------------------

	std::vector<unsigned char> bytes;

	//--------------------------------------------------------------------------
	// Read the file into memory
	FILE* pFile = nullptr;
	errno_t err = fopen_s(&pFile, pFilePath, "rb");

	if (0==err)
	{
		fseek(pFile, 0, SEEK_END);
		size_t length = ftell(pFile);	// get file size
		fseek(pFile, 0, SEEK_SET);

		bytes.resize( length );			// make sure buffer is large enough

		// Read in the file
		fread(&bytes[0], sizeof(unsigned char), bytes.size(), pFile);
		fclose(pFile);
	}

	if (bytes.size())
	{
		size_t file_offset = 0;	// File Cursor

		// Bytes are in the buffer, so let's start looking at what we have
		GSLA_Header* pHeader = (GSLA_Header*) &bytes[0];

		// Early out if things don't look right
		if (!pHeader->IsValid((unsigned int)bytes.size()))
			return;

		// Size in bytes for each frame in this animation
		m_frameSize = pHeader->frame_size;

		// pre-allocate all the frames
		for (unsigned int idx = 0; idx < pHeader->frame_count; ++idx)
		{
			m_pC1PixelMaps.push_back(new unsigned char[ m_frameSize ]);
		}

		//----------------------------------------------------------------------
		// Process Chunks as we encounter them
		file_offset += sizeof(GSLA_Header);

		// While we're not at the end of the file
		while (file_offset < bytes.size())
		{
			// This is pretty dumb, just get it done
			// These are the types I understand
			// every chunk is supposed to contain a value chunk_length
			// at offset +4, so that we can ignore ones we don't understand
			GSLA_INIT* pINIT = (GSLA_INIT*)&bytes[ file_offset ];
			GSLA_ANIM* pANIM = (GSLA_ANIM*)&bytes[ file_offset ];
			GSLA_CHUNK* pCHUNK = (GSLA_CHUNK*)&bytes[ file_offset ];

			if (pINIT->IsValid())
			{
				// We have an initial frame chunk
				UnpackInitialFrame(pINIT);
			}
			else if (pANIM->IsValid())
			{
				// We have a packed animation frames chunk
				UnpackAnimation(pANIM, pHeader);
			}

			file_offset += pCHUNK->chunk_length;

		}
	}
}

//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
//
// Unpack the initial frame, that's been packed with an empty initial dictionary
// So every byte of the buffer will be written out (no skip opcodes)
//
void GSLAFile::UnpackInitialFrame(GSLA_INIT* pINIT)
{
	unsigned char *pData = ((unsigned char*)pINIT) + sizeof(GSLA_INIT);

	unsigned char *pTargetBuffer = m_pC1PixelMaps[ 0 ]; // Data needs to be pre allocated

	//DecompressFrame(pTargetBuffer, pData);
}

//------------------------------------------------------------------------------
//
// Unpack the animation frame, assuming that the initial frame already exists
//
void GSLAFile::UnpackAnimation(GSLA_ANIM* pANIM, GSLA_Header* pHeader)
{
	unsigned char *pData = ((unsigned char*)pANIM) + sizeof(GSLA_ANIM);

	unsigned char *pTargetBuffer = m_pC1PixelMaps[ 0 ]; // Data needs to be pre allocated

	//DecompressAnim(pTargetBuffer, pData);
}
