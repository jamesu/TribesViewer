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
     
     if (version != 3)
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


