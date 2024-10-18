//-----------------------------------------------------------------------------
// Copyright (c) 2018 James S Urquhart.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
//-----------------------------------------------------------------------------

#include <stdint.h>
#include <stdio.h>
#include <strings.h>
#include <algorithm>
#include <string>
#include <vector>
#include <cmath>
#include <climits>
#include <unordered_map>
#include <slm/slmath.h>
#include "CommonData.h"

DarkstarPersistObject::NamedFuncMap DarkstarPersistObject::smNamedCreateFuncs;
DarkstarPersistObject::IDFuncMap DarkstarPersistObject::smIDCreateFuncs;

DarkstarPersistObject* DarkstarPersistObject::createFromStream(MemRStream& mem)
{
   IFFBlock block;
   uint32_t version = 0;
   mem.read(block);
   uint32_t start = mem.getPosition();
   std::string className;
   
   DarkstarPersistObject* obj;
   
   if (block.ident == IDENT_PERS)
   {
      mem.readSString(className);
      mem.read(version);
      
      obj = createClassByName(className);
   }
   else
   {
      // Try tag
      obj = createClassByTag(block.ident);
   }
   
   assert(obj);
   
   // Try reading obj
   if (obj && !obj->read(mem, version))
   {
      delete obj;
      obj = NULL;
   }
   
   mem.setPosition(start+block.getSize());
   return obj;
}

Palette::Palette() : mRemapData(NULL)
{
}
   
Palette::~Palette()
{
  if (mRemapData) free(mRemapData);
}
   
bool Palette::readMSPAL(MemRStream& mem)
{
  IFFBlock block;
  mem.read(block);
  
  if (block.ident != IDENT_RIFF)
  {
     return false;
  }
  
  mem.read(block);
  if (block.ident != IDENT_PAL)
  {
     return false;
  }
  
  uint16_t numColors, version;
  mem.read(numColors);
  mem.read(version);
  
  mPalettes.push_back(Data());
  Data& lp = mPalettes.back();
  memset(lp.colors, '\0', sizeof(lp.colors));
  lp.type = PALETTE_NOREMAP;
  
  uint32_t colsToRead = std::min<uint32_t>(numColors, 256);
  mem.read(colsToRead*4, lp.colors);
  mem.mPos += (numColors - colsToRead) * 4;
  
  return true;
}

uint32_t Palette::calcLookupSize(uint32_t type)
{
  const uint32_t baseSize = 256 + (4 * (256 * 4));
  switch(type)
  {
     case PALETTE_SHADEHAZE:
        return (256 * mShadeLevels * (mHazeLevels + 1)) + baseSize;
     case PALETTE_TRANSLUCENT:
     case PALETTE_ADDITIVE:
     case PALETTE_SUBTRACTIVE:
        return 65536 + baseSize;
     case PALETTE_NOREMAP:
        return 256 + baseSize;
     default:
        return 0;
  }
}

bool Palette::read(MemRStream& mem)
{
  IFFBlock block;
  mem.read(block);
  if (block.ident == IDENT_RIFF)
  {
     mem.setPosition(0);
     return readMSPAL(mem);
  }
  else if (block.ident == IDENT_PPAL)
  {
     mem.read(block); // head
     if (block.ident != IDENT_head)
        return false;
     
     uint8_t version;
     mem.read(version);
     
     if (version != 3 && version != 7)
        return false;
     
     uint16_t tmp;
     mem.read(tmp);
     
     uint8_t tmp2;
     mem.read(tmp2);
     mShadeShift = tmp2;
     mShadeLevels = 1 << mShadeShift;
     mHazeLevels = 0;
     
     uint32_t startPos = mem.getPosition();
     mem.read(block);
     
     //
     if (block.ident == IDENT_info)
     {
        block.seekToEnd(startPos, mem);
        mem.read(block);
     }
     
     if (block.ident != IDENT_data)
        return false;
     
     mPalettes.resize(1);
     Data* entry = &mPalettes[0];
     
     entry->type = PALETTE_NOREMAP;
     entry->index = -1;
     mem.read(entry->colors);
     
     // TODO: read remap chunks, should work the same as in PL98
  }
  else if (block.ident == IDENT_PL98)
  {
     mPalettes.resize(block.getRawSize());
     
     mem.read(mShadeShift);
     mShadeLevels = 1 << mShadeShift;
     mem.read(mHazeLevels);
     mem.read(mHazeColor);
     uint32_t sp = mem.getPosition();
     mem.read(mAllowedMatches);
     assert(mem.getPosition()-sp == 32);
     
     uint32_t lookupSize = 0;
     for (std::vector<Data>::iterator itr=mPalettes.begin(), itrEnd = mPalettes.end(); itr != itrEnd; itr++)
     {
        Data* entry = &*itr;
        *entry = Data();
        
        sp = mem.getPosition();
        mem.read(entry->colors);
        assert(mem.getPosition()-sp == 1024);
        sp = mem.getPosition();
        mem.read(entry->index);
        mem.read(entry->type);
        assert(mem.getPosition()-sp == 8);
        
        uint32_t palLookupSize = calcLookupSize(entry->type);
        lookupSize += palLookupSize;
     }
     
     mRemapData = (uint8_t*)malloc(lookupSize);
     mem.read(lookupSize, mRemapData);
     
     // Assign lookup ptrs
     // NOTE: data is stored as shadeMap+hazeMap[], remap(sh/tr/ad/sub)[], noremap[]
     uint8_t *pRemap = mRemapData;
     for (std::vector<Data>::iterator itr=mPalettes.begin(), itrEnd = mPalettes.end(); itr != itrEnd; itr++)
     {
        if (itr->type == PALETTE_SHADEHAZE)
        {
           const uint32_t sz1 = 256 * mShadeLevels * mHazeLevels;
           const uint32_t sz2 = 256 * mShadeLevels;
           
           itr->shadeMap = pRemap + sz1;
           pRemap += sz1;
           itr->hazeMap = pRemap + sz1;
           pRemap += sz2;
        }
        else if (itr->type == PALETTE_TRANSLUCENT || itr->type == PALETTE_ADDITIVE || itr->type == PALETTE_SUBTRACTIVE)
        {
           itr->transMap = pRemap;
           pRemap += 65536;
        }
     }
     
     for (std::vector<Data>::iterator itr=mPalettes.begin(), itrEnd = mPalettes.end(); itr != itrEnd; itr++)
     {
        if (itr->type == PALETTE_NOREMAP ||
            itr->type == PALETTE_COLORQUANT ||
            itr->type == PALETTE_ALPHAQUANT ||
            itr->type == PALETTE_ADDITIVEQUANT ||
            itr->type == PALETTE_SUBTRACTIVEQUANT)
           continue;
        
        itr->colIdx = pRemap; pRemap += 256;
        itr->colR = (float*)pRemap; pRemap += 256*4;
        itr->colG = (float*)pRemap; pRemap += 256*4;
        itr->colB = (float*)pRemap; pRemap += 256*4;
        itr->colA = (float*)pRemap; pRemap += 256*4;
     }
     
     for (std::vector<Data>::iterator itr=mPalettes.begin(), itrEnd = mPalettes.end(); itr != itrEnd; itr++)
     {
        if (itr->type != PALETTE_NOREMAP)
           continue;
        itr->colIdx = pRemap; pRemap += 256;
        itr->colR = (float*)pRemap; pRemap += 256*4;
        itr->colG = (float*)pRemap; pRemap += 256*4;
        itr->colB = (float*)pRemap; pRemap += 256*4;
        itr->colA = (float*)pRemap; pRemap += 256*4;
     }
     
     assert(pRemap - mRemapData == lookupSize);
     
     uint8_t weightPresent = 0;
     uint32_t dummy;
     mem.read(weightPresent);
     if (weightPresent)
     {
        mem.read(mColorWeights);
        mem.read(mWeightStart);
        mem.read(mWeightEnd);
     }
     
     mem.read(dummy);
  }
  
  return true;
}

Palette::Data* Palette::getPaletteByIndex(uint32_t idx)
{
  for (Data& dat : mPalettes)
  {
     if (dat.index == idx)
        return &dat;
  }
  
  return &mPalettes[0]; // fallback
}

Bitmap::Bitmap() : mData(NULL), mUserData(NULL), mPal(NULL), mBGR(false)
{;}

Bitmap::~Bitmap()
{
  if (mData) free(mData);
  if (mUserData) free(mUserData);
  if (mPal) delete mPal;
}

bool Bitmap::readMSBMP(MemRStream& mem)
{
  BITMAPFILEHEADER header;
  BITMAPINFOHEADER info_header;
  
  mBGR = true;
  mem.read(header);
  mem.read(info_header);
  mWidth = info_header.biWidth;
  mHeight = abs(info_header.biHeight);
  mBitDepth = info_header.biBitCount;
  mFlags = 0;
  mStride = getStride(mWidth);
  mMipLevels = 1;
  memset(mMips, '\0', sizeof(mMips));
  mPaletteIndex = (header.bfReserved1 == 0xf5f7 && header.bfReserved2 != 0xffff) ? (int32_t)header.bfReserved2 : -1;
  
  if ((header.bfType & 0xFFFF) != IDENT_BM00)
  {
     return false;
  }
  
  if(info_header.biBitCount == 8)
  {
     // Read pal
     mPal = new Palette();
     mPal->mPalettes.push_back(Palette::Data());
     Palette::Data& lp = mPal->mPalettes.back();
     
     memset(lp.colors, '\0', sizeof(lp.colors));
     lp.type = Palette::PALETTE_NOREMAP;
     
     uint32_t colsToRead = std::min<uint32_t>(info_header.biClrUsed, 256);
     mem.read(colsToRead*4, lp.colors);
     mem.mPos += (info_header.biClrUsed - colsToRead) * 4;
  }
  
  mData = (uint8_t*)malloc(mHeight * mStride);
  memset(mData, '\0', mHeight * mStride);
  mMips[0] = mData;
  
  for(uint32_t i = 0; i < info_header.biHeight; i++)
  {
     uint8_t *rowDest = getAddress(0, 0, info_header.biHeight - i - 1);
     mem.read(mStride, rowDest);
  }
  
  return true;
}

bool Bitmap::read(MemRStream& mem)
{
  IFFBlock block;
  mem.read(block);
  uint32_t expectedChunks=UINT_MAX-1;
  uint32_t version = 0;
  mPaletteIndex = -1;
  
  if ((block.ident & 0xFFFF) == IDENT_BM00)
  {
     mem.setPosition(0);
     return readMSBMP(mem);
  }
  else if (block.ident != IDENT_PBMP)
  {
     // Unlikely to be darkstar bmp
     return false;
  }
  
  while (!mem.isEOF() && expectedChunks != 0)
  {
     uint32_t startPos = mem.getPosition();
     mem.read(block);
     expectedChunks -= 1;
     
     switch (block.ident)
     {
        case IDENT_head:
           mem.read(version);
           mem.read(mWidth);
           mem.read(mHeight);
           mem.read(mBitDepth);
           mem.read(mFlags);
           expectedChunks = version & 0xFFFFFF;
           block.seekToEnd(startPos, mem);
           
           if (version >> 24 != 0)
              return false;
           
           break;
        case IDENT_DETL:
           mem.read(mMipLevels);
           block.seekToEnd(startPos, mem);
           break;
        case IDENT_piDX:
           mem.read(mPaletteIndex);
           block.seekToEnd(startPos, mem);
           break;
        case IDENT_data:
           if (mData) free(mData);
           mData = (uint8_t*)malloc(block.getSize());
           mem.read(block.getSize(), mData);
           block.seekToEnd(startPos, mem);
           break;
        case IDENT_RIFF:
           // Embedded MS palette
           if (mPal) delete mPal;
           mPal = new Palette();
           mem.setPosition(mem.getPosition()-4);
           
           if (!mPal->readMSPAL(mem))
           {
              return false;
           }
           
           block.seekToEnd(startPos, mem);
           break;
        default:
           block.seekToEnd(startPos, mem);
           break;
     }
  }
  
  mStride = getStride(mWidth);
  
  // Setup mips
  memset(mMips, '\0', sizeof(mMips));
  uint8_t* ptr = mData;
  uint32_t mipSize = mStride * mHeight;
  
  for (uint32_t i=0; i<mMipLevels; i++)
  {
     mMips[i] = ptr;
     ptr += mipSize;
     mipSize /= 4;
  }
  
  return true;
}

// NOTE: stripped down version of LZHUFF algorithm for read-only decoding

void LZH::lzh_unpack(int text_size, MemRStream& in_stream, MemRStream& out_stream)
{
   init_huff_and_tree();
   int r = BUF_SIZE - LOOK_AHEAD;
   text_buf.assign(BUF_SIZE + LOOK_AHEAD - 1, 0);
   int count = 0;
   
   while (count < text_size) 
   {
      int c = decode_char(in_stream);
      //printf("DECODE_CHAR %u\n", c);
      if (c < 256)
      {
         uint8_t cv = static_cast<uint8_t>(c);
         //printf("YIP %u\n", c);
         out_stream.write(cv);
         text_buf[r] = c;
         r = (r + 1) & (BUF_SIZE - 1);
         count++;
      } 
      else
      {
         //printf("YAP\n");
         int i = (r - decode_position(in_stream) - 1) & (BUF_SIZE - 1);
         int j = c - 255 + THRESHOLD;
         for (int k = 0; k < j; ++k) {
            const int rb_pos = (i + k) & (BUF_SIZE - 1);
            c = text_buf[rb_pos];
            uint8_t cv = static_cast<uint8_t>(c);
            //printf("WRITE %u\n", c);
            out_stream.write(cv);
            text_buf[r] = c;
            r = (r + 1) & (BUF_SIZE - 1);
            count++;
         }
      }
   }
}

void LZH::init_huff_and_tree()
{
   getbuf = 0;
   getlen = 0;
   putlen = 0;
   putbuf = 0;
   codesize = 0;
   match_length = 0;
   textsize = 0;
   start_huff();
}

void LZH::start_huff()
{
   freq.assign(TABLE_SIZE + 1, 0);
   prnt.assign(TABLE_SIZE + N_CHAR, 0);
   son.assign(TABLE_SIZE, 0);
   
   for (int i = 0; i < N_CHAR; i++)
   {
      freq[i] = 1;
      son[i] = (i + TABLE_SIZE) & 0xFFFF;
      prnt[i + TABLE_SIZE] = i;
   }
   
   int i = 0;
   int j = N_CHAR;
   while (j <= ROOT) 
   {
      freq[j] = freq[i] + freq[i + 1];
      son[j] = i & 0xFFFF;
      prnt[i] = prnt[i + 1] = j;
      i += 2;
      j += 1;
   }
   
   freq[TABLE_SIZE] = 0xffff;
   prnt[ROOT] = 0;
}

int LZH::decode_char(MemRStream& ios)
{
   int c = son[ROOT];
   while (c < TABLE_SIZE) 
   {
      c += get_bit(ios);
      c = son[c];
   }
   c -= TABLE_SIZE;
   update(c);
   return c;
}

int LZH::get_bit(MemRStream& ios)
{
   refill_byte_buf(ios);
   int bit = getbuf;
   getbuf <<= 1;
   getbuf &= 0xFFFF;
   getlen -= 1;
   return (bit >> 15) & 0x1;
}

int LZH::get_byte(MemRStream& ios)
{
   refill_byte_buf(ios);
   int byte = getbuf;
   getbuf <<= 8;
   getbuf &= 0xFFFF;
   getlen -= 8;
   return byte >> 8;
}

static uint32_t pcount = 0;
int LZH::decode_position(MemRStream& ios)
{
   int i = get_byte(ios);
   int ob = i;
   int j = decode_dlen(i);
   int c = D_CODE[i] << 6;
   
   j -= 2;
   for (int k = 0; k < j; ++k) 
   {
      i = (i << 1) + get_bit(ios);
   }
   
   //printf("DecodePosition %u -> %u [byte=%u len=%u]\n", pcount, c, ob, decode_dlen(ob));
   pcount++;
   
   return c | (i & 0x3f);
}

void LZH::refill_byte_buf(MemRStream& ios)
{
   while (getlen <= 8)
   {
      uint8_t byte;
      if (!ios.read(byte))
      {
         byte = 0;
      }
      getbuf |= static_cast<uint16_t>(byte) << (8 - getlen);
      getbuf &= 0xFFFF;
      getlen += 8;
   }
}

void LZH::update(int c)
{
   if (freq[ROOT] == MAX_FREQ)
   {
      reconst();
   }
   
   c = prnt[c + TABLE_SIZE];
   do
   {
      freq[c]++;
      int k = freq[c];
      int l = c + 1;
      
      if (k > freq[l])
      {
         while (k > freq[l]) l++;
         l--;
         std::swap(freq[c], freq[l]);
         int i = son[c];
         prnt[i] = l;
         if (i < TABLE_SIZE) prnt[i + 1] = l;
         int j = son[l];
         son[l] = i & 0xFFFF;
         prnt[j] = c;
         if (j < TABLE_SIZE) prnt[j + 1] = c;
         son[c] = j & 0xFFFF;
         c = l;
      }
      c = prnt[c];
   } while (c != 0);
}

void LZH::reconst()
{
   //printf("RECONST\n");
   int j = 0;
   for (int i = 0; i < TABLE_SIZE; i++)
   {
      if (son[i] >= TABLE_SIZE)
      {
         freq[j] = (freq[i] + 1) >> 1;
         son[j] = son[i];
         j++;
      }
   }
   int i = 0;
   for (int j = N_CHAR; j < TABLE_SIZE; j++)
   {
      int k = i + 1;
      int f = freq[j] = freq[i] + freq[k];
      k = j - 1;
      while (f < freq[k] && k >= 0) k--;
      k++;
      
      int l = (j - k) * sizeof(freq[0]);
      
      memmove(&freq[k+1], &freq[k], l);
      memmove(&son[k+1], &son[k], l);
      
      freq[k] = f;
      son[k] = i;
      i += 2;
   }
   for (int i = 0; i < TABLE_SIZE; i++)
   {
      int k = son[i];
      if (k >= TABLE_SIZE)
      {
         prnt[k] = i;
      }
      else
      {
         prnt[k] = i;
         prnt[k + 1] = i;
      }
   }
}

const uint8_t LZH::D_CODE[256] = {
   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
   0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
   0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
   0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
   0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
   0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
   0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
   0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
   0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
   0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
   0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
   0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
   0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09,
   0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A,
   0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B,
   0x0C, 0x0C, 0x0C, 0x0C, 0x0D, 0x0D, 0x0D, 0x0D,
   0x0E, 0x0E, 0x0E, 0x0E, 0x0F, 0x0F, 0x0F, 0x0F,
   0x10, 0x10, 0x10, 0x10, 0x11, 0x11, 0x11, 0x11,
   0x12, 0x12, 0x12, 0x12, 0x13, 0x13, 0x13, 0x13,
   0x14, 0x14, 0x14, 0x14, 0x15, 0x15, 0x15, 0x15,
   0x16, 0x16, 0x16, 0x16, 0x17, 0x17, 0x17, 0x17,
   0x18, 0x18, 0x19, 0x19, 0x1A, 0x1A, 0x1B, 0x1B,
   0x1C, 0x1C, 0x1D, 0x1D, 0x1E, 0x1E, 0x1F, 0x1F,
   0x20, 0x20, 0x21, 0x21, 0x22, 0x22, 0x23, 0x23,
   0x24, 0x24, 0x25, 0x25, 0x26, 0x26, 0x27, 0x27,
   0x28, 0x28, 0x29, 0x29, 0x2A, 0x2A, 0x2B, 0x2B,
   0x2C, 0x2C, 0x2D, 0x2D, 0x2E, 0x2E, 0x2F, 0x2F,
   0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
   0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F
};

