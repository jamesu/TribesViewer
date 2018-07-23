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
#include "SDL.h"
#include <stdio.h>
#include <strings.h>
#include <algorithm>
#include <string>
#include <vector>
#include <cmath>
#include <unordered_map>
#include <slm/slmath.h>
#include <GL/gl3w.h>

#include "imgui.h"
#include "imgui_impl_sdl.h"
#include "imgui_impl_opengl3.h"

#ifndef NO_BOOST
#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;
#else
#include <filesystem>
#endif

#ifdef HAVE_OPENGLES2
#define GL_GLEXT_PROTOTYPES
#include "SDL_opengles2.h"
#else
#include "SDL_opengl.h"
#endif

// Run of the mill quaternion interpolator
slm::quat CompatInterpolate( slm::quat const & q1,
                           slm::quat const & q2, float t )
{
   // calculate the cosine of the angle
   double cosOmega = q1.x * q2.x + q1.y * q2.y + q1.z * q2.z + q1.w * q2.w; // i.e. dot
   
   // adjust signs if necessary
   float sign2;
   if ( cosOmega < 0.0 )
   {
      cosOmega = -cosOmega;
      sign2 = -1.0f;
   }
   else
      sign2 = 1.0f;
   
   // calculate interpolating coeffs
   double scale1, scale2;
   if ( (1.0 - cosOmega) > 0.00001 )
   {
      // standard case
      double omega = acos(cosOmega);
      double sinOmega = sin(omega);
      scale1 = sin((1.0 - t) * omega) / sinOmega;
      scale2 = sign2 * sin(t * omega) / sinOmega;
   }
   else
   {
      // if quats are very close, just do linear interpolation
      scale1 = 1.0 - t;
      scale2 = sign2 * t;
   }
   
   // actually do the interpolation
   return slm::quat(float(scale1 * q1.x + scale2 * q2.x),
                    float(scale1 * q1.y + scale2 * q2.y),
                    float(scale1 * q1.z + scale2 * q2.z),
                    float(scale1 * q1.w + scale2 * q2.w));
}

#include "encodedNormals.h"

void CompatQuatSetMatrix(const slm::quat rot, slm::mat4 &outMat)
{
   if( rot.x*rot.x + rot.y*rot.y + rot.z*rot.z < 10E-20f)
   {
      outMat = slm::mat4(1);
      return;
   }
   
   float xs = rot.x * 2.0f;
   float ys = rot.y * 2.0f;
   float zs = rot.z * 2.0f;
   float wx = rot.w * xs;
   float wy = rot.w * ys;
   float wz = rot.w * zs;
   float xx = rot.x * xs;
   float xy = rot.x * ys;
   float xz = rot.x * zs;
   float yy = rot.y * ys;
   float yz = rot.y * zs;
   float zz = rot.z * zs;
   
   // r,c
   outMat[0] = slm::vec4(1.0f - (yy + zz),
                         xy - wz,
                         xz + wy,
                         0.0f);
   
   outMat[1] = slm::vec4(xy + wz,
                         1.0f - (xx + zz),
                         yz - wx,
                         0.0f);
   
   outMat[2] = slm::vec4(xz - wy,
                         yz + wx,
                         1.0f - (xx + yy),
                         0.0f);
   
   //outMat = slm::transpose(outMat);
   outMat[3] = slm::vec4(0.0f,0.0f,0.0f,1.0f);
}

class MemRStream;

// Root class for PERS objects
class DarkstarPersistObject
{
public:
   
   enum
   {
      IDENT_PERS = 1397900624
   };
   
   virtual ~DarkstarPersistObject(){;}
   virtual bool read(MemRStream &io, int version)=0;
   
   typedef std::unordered_map<uint32_t, std::function<DarkstarPersistObject*()> > IDFuncMap;
   typedef std::unordered_map<std::string, std::function<DarkstarPersistObject*()> > NamedFuncMap;
   static IDFuncMap smIDCreateFuncs;
   static NamedFuncMap smNamedCreateFuncs;
   static void initStatics();
   
   template<class T> static DarkstarPersistObject* _createClass() { return new T(); }
   static void registerClass(std::string className, std::function<DarkstarPersistObject*()> func)
   {
      smNamedCreateFuncs[className] = func;
   }
   
   static void registerClassID(uint32_t tag, std::function<DarkstarPersistObject*()> func)
   {
      smIDCreateFuncs[tag] = func;
   }
   
   static DarkstarPersistObject* createClassByName(std::string name)
   {
      NamedFuncMap::iterator itr = smNamedCreateFuncs.find(name);
      if (itr != smNamedCreateFuncs.end())
      {
         return itr->second();
      }
      return NULL;
   }
   
   static DarkstarPersistObject* createClassByTag(uint32_t tag)
   {
      IDFuncMap::iterator itr = smIDCreateFuncs.find(tag);
      if (itr != smIDCreateFuncs.end())
      {
         return itr->second();
      }
      return NULL;
   }
   
   static DarkstarPersistObject* createFromStream(MemRStream &mem);
};

DarkstarPersistObject::NamedFuncMap DarkstarPersistObject::smNamedCreateFuncs;
DarkstarPersistObject::IDFuncMap DarkstarPersistObject::smIDCreateFuncs;

class MemRStream
{
public:
   uint32_t mPos;
   uint32_t mSize;
   uint8_t* mPtr;
   
   bool mOwnPtr;
   
   MemRStream(uint32_t sz, void* ptr, bool ownPtr=false) : mPos(0), mSize(sz), mPtr((uint8_t*)ptr), mOwnPtr(ownPtr) {;}
   MemRStream(MemRStream &&other)
   {
      mPtr = other.mPtr;
      mPos = other.mPos;
      mSize = other.mSize;
      other.mOwnPtr = false;
      mOwnPtr = true;
   }
   MemRStream(MemRStream &other)
   {
      mPtr = other.mPtr;
      mPos = other.mPos;
      mSize = other.mSize;
      other.mOwnPtr = false;
      mOwnPtr = true;
   }
   MemRStream& operator=(MemRStream other)
   {
      mPtr = other.mPtr;
      mPos = other.mPos;
      mSize = other.mSize;
      other.mOwnPtr = false;
      mOwnPtr = true;
      return *this;
   }
   ~MemRStream()
   {
      if(mOwnPtr)
         free(mPtr);
   }
   
   // For array types
   template<class T, int N> inline bool read( T (&value)[N] )
   {
      if (mPos >= mSize || mPos+(sizeof(T)*N) > mSize)
         return false;
      
      memcpy(&value, mPtr+mPos, sizeof(T)*N);
      mPos += sizeof(T)*N;
      
      return true;
   }
   
   // For normal scalar types
   template<typename T> inline bool read(T &value)
   {
      T* tptr = (T*)(mPtr+mPos);
      if (mPos >= mSize || mPos+sizeof(T) > mSize)
         return false;
      
      value = *tptr;
      mPos += sizeof(T);
      
      return true;
   }
   
   inline bool read(uint32_t size, void* data)
   {
      if (mPos >= mSize || mPos+size > mSize)
         return false;
      
      memcpy(data, mPtr+mPos, size);
      mPos += size;
      
      return true;
   }
   
   inline bool readSString(std::string &outS)
   {
      uint16_t size;
      if (!read(size)) return false;
      
      int real_size = (size + 1) & (~1); // dword padded
      char *str = new char[real_size+1];
      if (read(real_size, str))
      {
         str[real_size] = '\0';
         outS = str;
         delete[] str;
         return true;
      }
      else
      {
         delete[] str;
         return false;
      }
   }
   
   inline void setPosition(uint32_t pos)
   {
      if (pos > mSize)
         return;
      mPos = pos;
   }
   
   inline uint32_t getPosition() { return mPos; }
   
   inline bool isEOF() { return mPos >= mSize; }
};

class IFFBlock
{
public:
   enum
   {
      ALIGN_DWORD = 0x80000000
   };
   
   uint32_t ident;
protected:
   uint32_t size;
   
public:
   IFFBlock() : ident(0), size(0) {;}
   
   inline uint32_t getSize() const
   {
      if (size & ALIGN_DWORD)
         return ((size & ~ALIGN_DWORD) + 3) & ~3;
      else
         return ( (size + 1) & (~1) );
   }
   
   inline uint32_t getRawSize() const { return size; }
   
   inline void seekToEnd(uint32_t startPos, MemRStream &mem)
   {
      mem.setPosition(startPos + getSize() + 8);
   }
};

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

class Palette
{
public:
   enum
   {
      IDENT_PL98 = 943279184,
      IDENT_PPAL = 1279348816,
      IDENT_PAL = 541868368,
      IDENT_RIFF = 1179011410,
      IDENT_head = 1684104552,
      IDENT_info = 1868983913,
      IDENT_data = 1635017060,
      IDENT_pspl = 1819308912,
      IDENT_ptpl = 1819309168,
      IDENT_hzpl = 1819309168
   };
   
   enum
   {
      PALETTE_NOREMAP = 0,
      PALETTE_SHADEHAZE = 1,
      PALETTE_TRANSLUCENT = 2,
      PALETTE_COLORQUANT = 3,
      PALETTE_ALPHAQUANT = 4,
      PALETTE_ADDITIVEQUANT = 5,
      PALETTE_ADDITIVE = 6,
      PALETTE_SUBTRACTIVEQUANT = 7,
      PALETTE_SUBTRACTIVE = 8
   };
   
   // Palette entry
   struct Data
   {
      int32_t index;
      uint32_t type;
      uint32_t colors[256];
      
      // Shade/Haze/Translucency mappings
      uint8_t *shadeMap;
      uint8_t *hazeMap;
      uint8_t *transMap;
      
      // Extra mappings
      uint8_t* colIdx;
      float* colR;
      float* colG;
      float* colB;
      float* colA;
      
      Data() : index(0), type(0), shadeMap(NULL), hazeMap(NULL), transMap(NULL), colIdx(0), colR(0), colG(0), colB(0), colA(0) {;}
      inline void lookupRGB(uint8_t idx, uint8_t &outR, uint8_t &outG, uint8_t &outB)
      {
         uint32_t col = colors[idx];
         outR = col & 0xFF;
         outG = (col >> 8) & 0xFF;
         outB = (col >> 16) & 0xFF;
      }
      inline void lookupRGBA(uint8_t idx, uint8_t &outR, uint8_t &outG, uint8_t &outB, uint8_t &outA)
      {
         uint32_t col = colors[idx];
         outR = col & 0xFF;
         outG = (col >> 8) & 0xFF;
         outB = (col >> 16) & 0xFF;
         outA = (col >> 24) & 0xFF;
      }
   };
   
   int32_t mShadeShift;
   int32_t mShadeLevels;
   int32_t mHazeLevels;
   int32_t mHazeColor;
   uint8_t mAllowedMatches[32];
   float mColorWeights[256];
   uint32_t mWeightStart;
   uint32_t mWeightEnd;
   uint8_t* mRemapData;
   
   std::vector<Data> mPalettes;
   
   Palette() : mRemapData(NULL)
   {
   }
   
   ~Palette()
   {
      if (mRemapData) free(mRemapData);
   }
   
   bool readMSPAL(MemRStream& mem)
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
   
   uint32_t calcLookupSize(uint32_t type)
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
   
   bool read(MemRStream& mem)
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
   
   Data* getPaletteByIndex(uint32_t idx)
   {
      for (Data& dat : mPalettes)
      {
         if (dat.index == idx)
            return &dat;
      }
      
      return &mPalettes[0]; // fallback
   }
};

class Bitmap
{
public:
   
   enum
   {
      IDENT_BM00 = 19778,
      IDENT_piDX = 1480878416,
      IDENT_PBMP = 1347240528,
      IDENT_head = 1684104552,
      IDENT_RIFF = 1179011410,
      IDENT_PAL  = 541868368,
      IDENT_DETL = 1280591172,
      IDENT_data = 1635017060
   };
   
   enum
   {
      FLAG_NORMAL = 0x0,
      FLAG_TRANSPARENT = 0x1,
      FLAG_FUZZY = 0x2,
      FLAG_TRANSLUCENT = 0x4,
      FLAG_OWN_MEM = 0x8,
      FLAG_ADDITIVE = 0x10,
      FLAG_SUBTRACTIVE = 0x20,
      FLAG_ALPHA8 = 0x40,
      
      MAX_MIPS = 9
   };
   
   // MS BMP Stuff
   struct RGBQUAD
   {
      uint8_t rgbBlue;
      uint8_t rgbGreen;
      uint8_t rgbRed;
      uint8_t rgbReserved;
   };
   
#pragma pack(2)
   struct BITMAPFILEHEADER
   {
      uint16_t bfType;
      uint32_t bfSize;
      uint16_t bfReserved1;
      uint16_t bfReserved2;
      uint32_t bfOffBits;
   };
#pragma pack()
   
#pragma pack(2)
   struct BITMAPINFOHEADER
   {
      uint32_t biSize;
      int32_t biWidth;
      int32_t biHeight;
      uint16_t biPlanes;
      uint16_t biBitCount;
      uint32_t biCompression;
      uint32_t biSizeImage;
      int32_t biXPelsPerMeter;
      int32_t biYPelsPerMeter;
      uint32_t biClrUsed;
      uint32_t biClrImportant;
   };
#pragma pack()
   //
   
   uint32_t mWidth;
   uint32_t mHeight;
   uint32_t mBitDepth;
   uint32_t mFlags;
   uint32_t mStride;
   
   uint32_t mMipLevels;
   int32_t mPaletteIndex;
   
   uint8_t* mData;
   char* mUserData;
   uint8_t* mMips[MAX_MIPS];
   
   Palette* mPal;
   
   Bitmap() : mData(NULL), mUserData(NULL), mPal(NULL) {;}
   ~Bitmap()
   {
      if (mData) free(mData);
      if (mUserData) free(mUserData);
      if (mPal) delete mPal;
   }
   
   bool readMSBMP(MemRStream& mem)
   {
      BITMAPFILEHEADER header;
      BITMAPINFOHEADER info_header;
      
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
   
   inline uint32_t getStride(uint32_t width) const { return 4 * ((width * mBitDepth + 31)/32); }
   
   uint8_t* getAddress(uint32_t mip, uint32_t x, uint32_t y)
   {
      assert(mip == 0);
      uint32_t stride = getStride(mWidth);
      return mMips[mip] + ((stride * y) + ((mBitDepth * x) / 8));
   }
   
   bool read(MemRStream& mem)
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
};

class Volume
{
public:
   enum
   {
      IDENT_PVOL = 1280267856,
      IDENT_vols = 1936486262,
      IDENT_voli = 1768714102
   };
   
   enum CompressType
   {
      COMPRESS_NONE=0,
      COMPRESS_RLE=1,         // not used in tribes
      COMPRESS_LZSS=2,        // not used in tribes
      COMPRESS_LZH=3          // not used in tribes
   };
   
#pragma pack(1)
   struct Entry
   {
      uint32_t ID;          // Tag ID
      int32_t pFilename;    // Filename pointer (rel to mStringTable)
      int32_t offset;       // Offset to VBLK chunk
      uint32_t size;        // Uncompressed size of file
      uint8_t compressType;
      
      inline const char* getFilename(const char* str) const { return pFilename >= 0 ? str + pFilename : ""; }
   };
#pragma pack()
   
   std::vector<Entry> mFiles;
   char* mStringData;
   FILE* mFilePtr;
   std::string mName;

   Volume() : mStringData(NULL), mFilePtr(NULL)
   {
   }
   
   ~Volume()
   {
      if (mStringData) free(mStringData);
      if (mFilePtr) fclose(mFilePtr);
   }
   
   bool read(FILE* fp)
   {
      IFFBlock block;
      assert(sizeof(Entry) == 17);
      
      if (mStringData) free(mStringData);
      mStringData = NULL;
      
      fread(&block, sizeof(IFFBlock), 1, fp);
      if (block.ident != IDENT_PVOL)
      {
         return false;
      }
      
      fseek(fp, block.getRawSize(), SEEK_SET);
      fread(&block, sizeof(IFFBlock), 1, fp);
      if (block.ident != IDENT_vols)
      {
         return false;
      }
      
      uint32_t real_size = block.getSize();
      mStringData = (char*)malloc(real_size);
      if (fread(mStringData, real_size, 1, fp) != 1)
      {
         return false;
      }
      
      fread(&block, sizeof(IFFBlock), 1, fp);
      if (block.ident != IDENT_voli)
      {
         return false;
      }
      
      uint32_t numItems = block.getSize() / sizeof(Entry);
      mFiles.resize(numItems);
      
      if (fread(&mFiles[0], sizeof(Entry), numItems, fp) != numItems)
      {
         return false;
      }
      
      return true;
   }
   
   bool openStream(FILE* fp, const char* filename, MemRStream& outStream)
   {
      for (std::vector<Entry>::const_iterator itr = mFiles.begin(), itrEnd = mFiles.end(); itr != itrEnd; itr++)
      {
         if (strcasecmp(filename, itr->getFilename(mStringData)) == 0)
         {
            fseek(fp, itr->offset+8, SEEK_SET); // skip past VBLK header
            uint8_t* data = (uint8_t*)malloc(itr->size);
            if (fread(data, itr->size, 1, fp) == 0)
            {
               free(data);
               return false;
            }
            assert(itr->compressType == 0); // TODO: handle compression variants
            outStream = MemRStream(itr->size, data, true);
            return true;
         }
      }
      
      return NULL;
   }
};

class ResManager
{
public:
   
   struct EnumEntry
   {
      std::string filename;
      uint32_t mountIdx;
      
      EnumEntry() {;}
      EnumEntry(const char *name, uint32_t m) : filename(name), mountIdx(m) {;}
   };
   
   std::vector<Volume*> mVolumes;
   std::vector<std::string> mPaths;
   
   void addVolume(const char *filename)
   {
      FILE* fp = fopen(filename, "rb");
      if (fp)
      {
         Volume* vol = new Volume();
         if (!vol->read(fp))
         {
            delete vol;
            return;
         }
         
         vol->mFilePtr = fp;
         vol->mName = filename;
         mVolumes.push_back(vol);
      }
   }
   
   bool openFile(const char *filename, MemRStream &stream, int32_t forceMount=-1)
   {
      // Check cwd
      int count = 0;
      for (std::string &path: mPaths)
      {
         if (forceMount >= 0 && count != forceMount)
         {
            count++;
            continue;
         }
         char buffer[PATH_MAX];
         snprintf(buffer, PATH_MAX, "%s/%s", path.c_str(), filename);
         FILE* fp = fopen(buffer, "rb");
         if (fp)
         {
            fseek(fp, 0, SEEK_END);
            uint32_t size = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            uint8_t* data = (uint8_t*)malloc(size);
            if (fread(data, size, 1, fp) == 1)
            {
               stream = MemRStream(size, data, true);
               fclose(fp);
               printf("Loaded local file %s\n", buffer);
               return true;
            }
            free(data);
            fclose(fp);
            return false;
         }
         count++;
      }
      
      // Scan volumes
      for (Volume* vol: mVolumes)
      {
         if (forceMount >= 0 && count != forceMount)
         {
            count++;
            continue;
         }
         if (vol->openStream(vol->mFilePtr, filename, stream))
         {
            printf("Loaded volume file %s from volume\n", filename);
            return true;
         }
         count++;
      }
      
      return false;
   }
   
   DarkstarPersistObject* openObject(const char *filename, int32_t forceMount=-1)
   {
      DarkstarPersistObject* obj = NULL;
      MemRStream mem(0, NULL);
      if (openFile(filename, mem, forceMount))
      {
         obj = DarkstarPersistObject::createFromStream(mem);
      }
      return obj;
   }
   
   void enumerateVolume(uint32_t idx, std::vector<EnumEntry> &outList, std::string *restrictExt)
   {
      for (Volume::Entry &e : mVolumes[idx]->mFiles)
      {
         if (restrictExt)
         {
            std::string ext = fs::extension(e.getFilename(mVolumes[idx]->mStringData));
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != *restrictExt)
               continue;
         }
         outList.emplace_back(EnumEntry(e.getFilename(mVolumes[idx]->mStringData), mPaths.size()+idx));
      }
   }
   
   void enumeratePath(uint32_t idx, std::vector<EnumEntry> &outList, std::string *restrictExt)
   {
      for (fs::directory_entry &itr : fs::directory_iterator(mPaths[idx]))
      {
         if (restrictExt)
         {
            std::string ext = fs::extension(itr.path().filename().c_str());
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != *restrictExt)
               continue;
         }
         outList.emplace_back(EnumEntry(itr.path().filename().c_str(), idx));
      }
   }
   
   void enumerateFiles(std::vector<EnumEntry> &outList, int restrictIdx=-1, std::string *restrictExt=NULL)
   {
      for (int i=0; i<mPaths.size(); i++)
      {
         if (restrictIdx >= 0 && restrictIdx != i)
            continue;
         enumeratePath(i, outList, restrictExt);
      }
      for (int i=0; i<mVolumes.size(); i++)
      {
         if (restrictIdx >= 0 && restrictIdx != mPaths.size()+i)
            continue;
         enumerateVolume(i, outList, restrictExt);
      }
   }
   
   void enumerateSearchPaths(std::vector<const char*> &outList)
   {
      for (int i=0; i<mPaths.size(); i++)
      {
         outList.push_back(mPaths[i].c_str());
      }
      for (int i=0; i<mVolumes.size(); i++)
      {
         outList.push_back(mVolumes[i]->mName.c_str());
      }
   }
   
   const char *getMountName(uint32_t idx)
   {
      if (idx < mPaths.size())
         return mPaths[idx].c_str();
      idx -= mPaths.size();
      if (idx < mVolumes.size())
         return mVolumes[idx]->mName.c_str();
      return "NULL";
   }
};

class Material
{
public:
   
   enum
   {
      NAMESIZE_V1 = 16,
      NAMESIZE_V2 = 32
   };
   
   enum
   {
      FLAG_MASK = 0xF,
      FLAG_NULL = 0x0,
      FLAG_PALETTE = 0x1,
      FLAG_RGB = 0x2,
      FLAG_TEXTURE = 0x3,
      FLAG_SHADING_MASK = 0xF00,
      FLAG_SHADING_NONE = 0x100,
      FLAG_SHADING_FLAT = 0x200,
      FLAG_SHADING_SMOOTH = 0x300,
      FLAG_TEXTURE_MASK = 0xF000,
      FLAG_TEXTURE_TRANSPARENT = 0x1000,
      FLAG_TEXTURE_TRANSLUCENT = 0x1000
   };
   
   uint32_t mFlags;
   float mAlpha;
   uint32_t mIndex;
   uint8_t mRGB[4]; // last is padding
   uint8_t mFilename[NAMESIZE_V2];
   // v3+
   uint32_t mType; // See setMaterialProperty in the script files for a type list
   float mElasticity;
   float mFriction;
   // v4+
   uint32_t mUseDefaultProps;
   
   Material()
   {
      memset(this, '\0', sizeof(Material));
   }
   
   bool read(MemRStream &mem, int version)
   {
      mem.read(mFlags);
      mem.read(mAlpha);
      mem.read(mIndex);
      mem.read(mRGB);
      mem.read(version < 2 ? NAMESIZE_V1 : NAMESIZE_V2, mFilename);
      if (version == 1 || version > 2)
      {
         mem.read(mType);
         mem.read(mElasticity);
         mem.read(mFriction);
      }
      if (version != 2 && version != 3)
      {
         mem.read(mUseDefaultProps);
      }
      else
      {
         mUseDefaultProps = 1;
      }
      return true;
   }
};

class MaterialList : public DarkstarPersistObject
{
public:
   
   uint32_t mNumDetails;
   std::vector<Material> mMaterials;
   
   MaterialList()
   {
   }
   
   virtual ~MaterialList()
   {
   }
   
   bool read(MemRStream &stream, int version)
   {
      uint32_t sz;
      stream.read(mNumDetails);
      stream.read(sz);
      mMaterials.resize(sz * mNumDetails);
      for (size_t i=0, sz=mMaterials.size(); i<sz; i++)
      {
         mMaterials[i].read(stream, version);
      }
      return true;
   }
};

// 16-bit quat type (same as torque)
struct Quat16
{
   enum { MAX_VAL = 0x7fff };
   
   int16_t x, y, z, w;
   
   Quat16() : x(0),y(0),z(0),w(0) {;}
   
   Quat16(const slm::quat &src)
   {
      x = src.x * float(MAX_VAL);
      y = src.y * float(MAX_VAL);
      z = src.z * float(MAX_VAL);
      w = src.w * float(MAX_VAL);
   }
   
   slm::quat toQuat() const
   {
      slm::quat outQuat;
      outQuat.x = float(x) / float(MAX_VAL);
      outQuat.y = float(y) / float(MAX_VAL);
      outQuat.z = float(z) / float(MAX_VAL);
      outQuat.w = float(w) / float(MAX_VAL);
      return outQuat;
   }
   
   bool operator==(const Quat16 &q) const { return( x == q.x && y == q.y && z == q.z && w == q.w ); }
   bool operator!=( const Quat16 & q ) const { return !(*this == q); }
};

class CelAnimMesh : public DarkstarPersistObject
{
public:
   
   struct PackedVertex
   {
      uint8_t x,y,z,normal;
   };
   
   struct VertexIndexPair
   {
      int32_t vi;
      int32_t ti;
      
      VertexIndexPair() {;}
      VertexIndexPair(int32_t v, int32_t t) : vi(v), ti(t) {;}
      uint64_t getHashCode() { return ((uint64_t)vi) | (((uint64_t)ti) << 32); }
      
      bool operator==(const VertexIndexPair &other) { return vi == other.vi && ti == other.ti; }
      bool operator!=(const VertexIndexPair &other) { return vi != other.vi || ti != other.ti; }
   };
   
   struct Triangle
   {
      uint16_t i[3];
      Triangle() {;}
   };
   
   struct Face
   {
      VertexIndexPair verts[3];
      int32_t mat;
   };
   
   struct Frame
   {
      int32_t firstVert;
      slm::vec3 scale;
      slm::vec3 origin;
   };
   
   struct Prim
   {
      uint32_t startVerts;
      uint32_t startInds;
      uint32_t numVerts;
      uint32_t numInds;
      int32_t  mat;
      
      Prim() : startVerts(0), numVerts(0), startInds(0), numInds(0), mat(-1) {;}
   };
   
   int32_t mVertsPerFrame;        // used when key changes
   int32_t mTextureVertsPerFrame; // used when matIndex changes
   
   slm::vec3 mScale;
   slm::vec3 mOrigin;
   
   float mRadius;
   
   std::vector<PackedVertex> mVerts;
   std::vector<slm::vec2> mTexVerts;
   std::vector<Face> mFaces;
   std::vector<Frame> mFrames;
   std::vector<uint32_t> mFixedFrameOffsets;
   
   CelAnimMesh()
   {
   }
   
   virtual ~CelAnimMesh()
   {
   }
   
   // Generates mapping used to construct final buffers
   // NOTE: could optimize texVerts so more pairs are reused. (i.e. by rebuilding index pairs)
   void unpackVertStructure(std::vector<uint32_t> &outVerts, std::vector<uint32_t> &outTexVerts, std::vector<Triangle> &outTris, std::vector<Prim> &outPrims)
   {
      Prim currentPrim;
      std::unordered_map<uint64_t, uint32_t> vtxToVert;
      
      //assert(mFrames[0].firstVert == 0);
      
      for (auto fi = mFaces.begin(), fe = mFaces.end(); fi != fe; fi++)
      {
         Triangle outTriangle;
         
         if (currentPrim.numInds != 0 && currentPrim.mat != fi->mat)
         {
            outPrims.push_back(currentPrim);
            currentPrim.numInds = 0;
         }
         
         if (currentPrim.numInds == 0)
         {
            currentPrim.startInds = (uint32_t)outTris.size()*3;
            currentPrim.startVerts = 0;//(uint32_t)outVerts.size();
            currentPrim.numVerts = 0;
            currentPrim.mat = fi->mat;
            vtxToVert.clear();
         }
         
         for (int i=0; i<3; i++)
         {
            auto itr = vtxToVert.find(fi->verts[i].getHashCode());
            uint32_t idx = 0;
            
            if (itr == vtxToVert.end())
            {
               // vert hasn't been converted yet
               idx = (uint32_t)outVerts.size();
               vtxToVert[fi->verts[i].getHashCode()] = idx;
               
               outVerts.push_back(fi->verts[i].vi);
               outTexVerts.push_back(fi->verts[i].ti);
               currentPrim.numVerts++;
            }
            else
            {
               // vert converted already
               idx = itr->second;
               assert(outVerts[itr->second] == fi->verts[i].vi);
            }
            assert(idx < 0xFFFF);
            outTriangle.i[i] = (uint16_t)idx;
         }
         
         outTris.push_back(outTriangle);
         currentPrim.numInds += 3;
      }
      
      if (currentPrim.numInds != 0)
      {
         outPrims.push_back(currentPrim);
      }
   }
   
   bool read(MemRStream &mem, int version)
   {
      int32_t numVerts=0;
      int32_t numFaces=0;
      int32_t numTexVerts=0;
      int32_t numFrames=0;
      mVertsPerFrame = 0;
      mTextureVertsPerFrame = 0;
      
      mem.read(numVerts);
      mem.read(mVertsPerFrame);
      mem.read(numTexVerts);
      mem.read(numFaces);
      mem.read(numFrames);
      
      if (version >= 2)
         mem.read(mTextureVertsPerFrame);
      else
         mTextureVertsPerFrame = numTexVerts;
      
      slm::vec3 v2scale;
      slm::vec3 v2origin;
      if (version < 3)
      {
         mem.read(v2scale);
         mem.read(v2origin);
      }
      
      mem.read(mRadius);
      
      mVerts.resize(numVerts);
      mem.read(numVerts * sizeof(PackedVertex), &mVerts[0]);
      mTexVerts.resize(numTexVerts);
      mem.read(numTexVerts * sizeof(slm::vec2), &mTexVerts[0]);
      mFaces.resize(numFaces);
      mem.read(numFaces * sizeof(Face), &mFaces[0]);
      
      mFrames.resize(numFrames);
      if (version < 3)
      {
         if (numFrames == 0)
         {
            Frame f;
            f.firstVert = 0;
            f.scale = v2scale;
            f.origin = v2origin;
            mFrames.push_back(f);
         }
         else
         {
            for (int i=0; i<numFrames; i++)
            {
               Frame* dest = &mFrames[i];
               mem.read(dest->firstVert);
               dest->scale = v2scale;
               dest->origin = v2origin;
            }
         }
      }
      else
      {
         mem.read(numFrames * sizeof(Frame), &mFrames[0]);
      }
      
      return true;
   }
};


class Shape : public DarkstarPersistObject
{
public:
   
   struct Transform
   {
      Quat16 rot;
      slm::vec3 pos;
   };
   
   enum
   {
      KEYFRAME_FRAME_MATTERS = 1<<12,
      KEYFRAME_MAT_MATTERS = 1<<13,
      KEYFRAME_VIS_MATTERS = 1<<14,
      KEYFRAME_VIS = 1<<15,
      KEYFRAME_MAT_MASK = 0x0FFF,
      
      KEYFRAME_VIS_V2 = 1<<31,
      KEYFRAME_VALID_V2 = 1<<30,
      KEYFRAME_KEY_MASK_V2 = 0x3FFFFFFF,
      
      KEYFRAME_VIS_MATTERS_V7 = 1<<30,
      KEYFRAME_MAT_MATTERS_V7 = 1<<29,
      KEYFRAME_FRAME_MATTERS_V7 = 1<<28,
      KEYFRAME_MAT_MASK_V7 = 0x0FFFFFFF,
   };
   
   struct Keyframe
   {
      float pos;
      uint16_t key; // shape/mesh idx
      uint16_t matIndex; // includes flags
   };
   
   struct Sequence
   {
      int32_t name;
      int32_t cyclic;
      float duration;
      int32_t priority;
      int32_t firstTriggerFrame;
      int32_t numTriggerFrames;
      int32_t numIFLSubSequences;
      int32_t firstIFLSubSequence;
   };
   
   struct SubSequence
   {
      int16_t sequenceIdx;
      int16_t numKeyFrames;
      int16_t firstKeyFrame;
   };
   
   struct Transition
   {
      int32_t startSequence;
      int32_t endSequence;
      float startPosition;
      float endPosition;
      float duration;
      Transform transform;   // this seems to be user configurable
   };
   
   struct Node
   {
      int16_t name;
      int16_t parent;
      int16_t numSubSequences;
      int16_t firstSubSequence;
      int16_t defaultTransform; // start transform index
   };
   
   enum ObjectFlags
   {
      OBJECT_INVISIBLE_DEFAULT = 0x1
   };
   
   struct Object
   {
      int16_t name;
      uint16_t flags;
      int32_t meshIndex;
      int16_t nodeIndex;
      slm::vec3 offset;   // relative to attached node
      int16_t numSubSequences;
      int16_t firstSubSequence;
   };
   
   struct Detail
   {
      int32_t rootNode;
      float size;
   };
   
   struct FrameTrigger
   {
      float pos;
      int32_t value;
   };
   
   struct NodeSortInfo
   {
      uint32_t nodeIdx;
      int32_t parentIdx;
      
      NodeSortInfo() {;}
      NodeSortInfo(uint32_t nidx, int32_t pidx) : nodeIdx(nidx), parentIdx(pidx) {;}
      bool operator==(const NodeSortInfo &other) { return nodeIdx == other.nodeIdx && parentIdx == other.parentIdx; }
   };
   
   struct NodeChildInfo
   {
      int32_t firstChild;
      int32_t numChildren;
      
      NodeChildInfo() : firstChild(-1), numChildren(0) {;}
   };
   
   // Main data
   float mRadius;
   slm::vec3 mCenter;
   slm::vec3 mMinBounds;
   slm::vec3 mMaxBounds;
   
   std::vector<Node> mNodes;
   std::vector<Sequence> mSequences;
   std::vector<SubSequence> mSubSequences;
   std::vector<Keyframe> mKeyframes;
   std::vector<Transform> mTransforms;
   std::vector<Object> mObjects;
   std::vector<Detail> mDetails;
   std::vector<Transition> mTransitions;
   std::vector<FrameTrigger> mFrameTriggers;
   std::vector<CelAnimMesh*> mMeshes;
   std::vector<std::string> mNames;
   
   MaterialList* mMaterials;
   int32_t mDefaultMaterials;
   int32_t mAlwaysNode;
   
   // Runtime info
   std::vector<NodeChildInfo> mNodeChildren;
   std::vector<uint32_t> mNodeChildIds;
   
   Shape() : mMaterials(NULL)
   {
   }
   
   virtual ~Shape()
   {
      if (mMaterials) delete mMaterials;
   }
   
   int findName(const char *name)
   {
      for (int i=0, sz = mNames.size(); i<sz; i++)
      {
         if (strcasecmp(name, mNames[i].c_str()) == 0)
            return i;
      }
      return -1;
   }
   
   const char *getName(int32_t idx)
   {
      return mNames[idx].c_str();
   }
   
   inline void readV6Transform(MemRStream &mem, Transform &outXfm)
   {
      slm::quat rot;
      slm::vec3 scale;
      mem.read(rot);
      mem.read(outXfm.pos);
      mem.read(scale);
      outXfm.rot = Quat16(rot);
   }
   
   inline void readV7Transform(MemRStream &mem, Transform &outXfm)
   {
      slm::vec3 scale;
      mem.read(outXfm.rot);
      mem.read(outXfm.pos);
      mem.read(scale);
   }
   
   void setupNodeList()
   {
      // Setup child node lists
      std::vector<NodeSortInfo> sortedNodes;
      sortedNodes.resize(mNodes.size());
      for (size_t i=0, sz = sortedNodes.size(); i<sz; i++)
      {
         sortedNodes[i] = NodeSortInfo((uint32_t)i, mNodes[i].parent);
         assert(mNodes[i].parent < (int32_t)sortedNodes.size());
      }
      
      // Nodes will be sorted by their parent
      std::sort(sortedNodes.begin(), sortedNodes.end(), [](const NodeSortInfo& a, const NodeSortInfo& b) {
         if (a.parentIdx == b.parentIdx)
            return a.nodeIdx < b.nodeIdx;
         else
            return a.parentIdx < b.parentIdx;
      });
      
      mNodeChildren.resize(sortedNodes.size()+1);
      mNodeChildIds.reserve(sortedNodes.size());
      
      for (size_t i=0, sz = sortedNodes.size(); i<sz; i++)
      {
         int32_t currentParent = sortedNodes[i].parentIdx;
         NodeChildInfo &childInfo = mNodeChildren[currentParent+1];
         childInfo.firstChild = (uint32_t)mNodeChildIds.size();
         for (i=i; i<sz; i++)
         {
            if (sortedNodes[i].parentIdx != currentParent) // On next parent
            {
               i--; // need to scan this node again
               
               break;
            }
            mNodeChildIds.push_back(sortedNodes[i].nodeIdx);
         }
         
         childInfo.numChildren = (uint32_t)(mNodeChildIds.size() - childInfo.firstChild);
      }
   }
   
   bool read(MemRStream &mem, int version)
   {
      uint32_t numNodes = 0;
      uint32_t numSequences = 0;
      uint32_t numSubSequences = 0;
      uint32_t numKeyframes = 0;
      uint32_t numTransforms = 0;
      uint32_t numNames = 0;
      uint32_t numObjects = 0;
      uint32_t numDetails = 0;
      uint32_t numMeshes = 0;
      uint32_t numTransitions = 0;
      uint32_t numFrameTriggers = 0;
      
      mAlwaysNode = -1;
      mDefaultMaterials = 0;
      
      mem.read(numNodes);
      mem.read(numSequences);
      mem.read(numSubSequences);
      mem.read(numKeyframes);
      mem.read(numTransforms);
      mem.read(numNames);
      mem.read(numObjects);
      mem.read(numDetails);
      mem.read(numMeshes);
      
      if (version >= 2) mem.read(numTransitions);
      if (version >= 4) mem.read(numFrameTriggers);
      
      mem.read(mRadius);
      mem.read(mCenter);
      
      if (version > 7)
      {
         mem.read(mMinBounds);
         mem.read(mMaxBounds);
      }
      else
      {
         mMinBounds = mCenter + (slm::vec3(-1,-1,-1) * mRadius);
         mMaxBounds = mCenter + (slm::vec3(1,1,1) * mRadius);
      }
      
      // Arrays
      
      mNodes.resize(numNodes);
      if (version <= 7)
      {
         for (int i=0; i<numNodes; i++)
         {
            Node* dest = &mNodes[i];
            int32_t tmp; mem.read(tmp); dest->name = tmp;
            mem.read(tmp); dest->parent = tmp;
            mem.read(tmp); dest->numSubSequences = tmp;
            mem.read(tmp); dest->firstSubSequence = tmp;
            mem.read(tmp); dest->defaultTransform = tmp;
         }
      }
      else
      {
         mem.read( sizeof(Node)*numNodes, &mNodes[0]);
      }
      
      mSequences.resize(numSequences);
      if (version >= 5)
      {
         mem.read( sizeof(Sequence)*numSequences, &mSequences[0]);
      }
      else if (version >= 4)
      {
         for (int i=0; i<numSequences; i++)
         {
            Sequence* dest = &mSequences[i];
            mem.read(dest->name);
            mem.read(dest->cyclic);
            mem.read(dest->duration);
            mem.read(dest->priority);
            mem.read(dest->firstTriggerFrame);
            mem.read(dest->numTriggerFrames);
            dest->numIFLSubSequences = dest->numIFLSubSequences = 0;
         }
      }
      else
      {
         for (int i=0; i<numSequences; i++)
         {
            Sequence* dest = &mSequences[i];
            mem.read(dest->name);
            mem.read(dest->cyclic);
            mem.read(dest->duration);
            mem.read(dest->priority);
            dest->numTriggerFrames = dest->numTriggerFrames = dest->numIFLSubSequences = dest->numIFLSubSequences = 0;
         }
      }
      
      // SubSequences
      mSubSequences.resize(numSubSequences);
      if (version <= 7)
      {
         for (int i=0; i<numSubSequences; i++)
         {
            SubSequence* dest = &mSubSequences[i];
            int32_t tmp=0;
            mem.read(tmp); dest->sequenceIdx = tmp;
            mem.read(tmp); dest->numKeyFrames = tmp;
            mem.read(tmp); dest->firstKeyFrame = tmp;
         }
      }
      else
      {
         mem.read( sizeof(SubSequence)*numSubSequences, &mSubSequences[0]);
      }
      
      // Keyframes
      mKeyframes.resize(numKeyframes);
      if (version < 3)
      {
         for (int i=0; i<numKeyframes; i++)
         {
            Keyframe* dest = &mKeyframes[i];
            mem.read(dest->pos);
            uint32_t tmp; mem.read(tmp);
            dest->key = tmp & KEYFRAME_KEY_MASK_V2;
            dest->matIndex = KEYFRAME_FRAME_MATTERS;
            if (!(tmp & KEYFRAME_VALID_V2)) dest->matIndex |= KEYFRAME_VIS_MATTERS;
            if (tmp & KEYFRAME_VIS_V2) dest->matIndex |= KEYFRAME_VIS;
         }
      }
      else if (version <= 7)
      {
         for (int i=0; i<numKeyframes; i++)
         {
            Keyframe* dest = &mKeyframes[i];
            mem.read(dest->pos);
            uint32_t tmp; mem.read(tmp); dest->key = tmp;
            mem.read(tmp); dest->matIndex = tmp & KEYFRAME_MAT_MASK_V7;
            if (tmp & KEYFRAME_VIS_V2) dest->matIndex |= KEYFRAME_VIS;
            if (tmp & KEYFRAME_VIS_MATTERS_V7) dest->matIndex |= KEYFRAME_VIS_MATTERS;
            if (tmp & KEYFRAME_FRAME_MATTERS_V7) dest->matIndex |= KEYFRAME_FRAME_MATTERS;
            if (tmp & KEYFRAME_MAT_MATTERS_V7) dest->matIndex |= KEYFRAME_MAT_MATTERS;
         }
      }
      else
      {
         mem.read( sizeof(Keyframe)*numKeyframes, &mKeyframes[0]);
      }
      
      // Transforms
      mTransforms.resize(numTransforms);
      if (version < 7)
      {
         for (int i=0; i<numTransforms; i++)
         {
            Transform* dest = &mTransforms[i];
            readV6Transform(mem, *dest);
         }
      }
      else if (version == 7)
      {
         for (int i=0; i<numTransforms; i++)
         {
            Transform* dest = &mTransforms[i];
            readV7Transform(mem, *dest);
         }
      }
      else
      {
         mem.read( sizeof(Transform)*numTransforms, &mTransforms[0]);
      }
      
      mNames.resize(numNames);
      char* tmpName = new char[numNames*24];
      mem.read(numNames*24, tmpName);
      for (int i=0; i<numNames; i++)
      {
         char* name = &tmpName[i*24];
         mNames[i] = std::string(name);
      }
      delete[] tmpName;
      
      // Objects
      mObjects.resize(numObjects);
      if (version <= 7)
      {
         for (int i=0; i<numObjects; i++)
         {
            Object* dest = &mObjects[i];
            mem.read(dest->name);
            mem.read(dest->flags);
            mem.read(dest->meshIndex);
            int32_t tmpi=0;
            mem.read(tmpi); dest->nodeIndex = tmpi;
            mem.setPosition(mem.mPos + sizeof(uint32_t) + (sizeof(float)*3*3)); // Skip past flags and rotm
            mem.read(dest->offset);
            mem.read(tmpi); dest->numSubSequences = tmpi;
            mem.read(tmpi); dest->firstSubSequence = tmpi;
         }
      }
      else
      {
         mem.read( sizeof(Object)*numObjects, &mObjects[0]);
      }
      
      // Details
      mDetails.resize(numDetails);
      mem.read(numDetails * sizeof(Detail), &mDetails[0]);
      
      // Transitions
      if (version >= 2)
      {
         mTransitions.resize(numTransitions);
         if (version < 7)
         {
            for (int i=0; i<numTransitions; i++)
            {
               Transition* dest = &mTransitions[i];
               mem.read(dest->startSequence);
               mem.read(dest->endSequence);
               mem.read(dest->startPosition);
               mem.read(dest->endPosition);
               mem.read(dest->duration);
               readV6Transform(mem, dest->transform);
            }
         }
         else if (version == 7)
         {
            for (int i=0; i<numTransitions; i++)
            {
               Transition* dest = &mTransitions[i];
               mem.read(dest->startSequence);
               mem.read(dest->endSequence);
               mem.read(dest->startPosition);
               mem.read(dest->endPosition);
               mem.read(dest->duration);
               readV7Transform(mem, dest->transform);
            }
         }
         else
         {
            mem.read( sizeof(Transition)*numTransitions, &mTransitions[0]);
         }
      }
      
      // Triggers
      if (version >= 4)
      {
         mFrameTriggers.resize(numFrameTriggers);
         mem.read(numFrameTriggers * sizeof(FrameTrigger), &mFrameTriggers[0]);
      }
      
      if (version >= 5)
         mem.read(mDefaultMaterials);
      if (version >= 6)
         mem.read(mAlwaysNode);
      
      // Meshes
      mMeshes.resize(numMeshes);
      for (int i=0; i<numMeshes; i++)
      {
         mMeshes[i] = (CelAnimMesh*)DarkstarPersistObject::createFromStream(mem);
      }
      
      uint32_t hasMaterials;
      mem.read(hasMaterials);
      
      if (hasMaterials)
      {
         mMaterials = (MaterialList*)DarkstarPersistObject::createFromStream(mem);
      }
      
      setupNodeList();
      
      return true;
   }
};

void DarkstarPersistObject::initStatics()
{
   registerClass("TS::MaterialList", &_createClass<MaterialList>);
   registerClass("TS::Shape", &_createClass<Shape>);
   registerClass("TS::CelAnimMesh", &_createClass<CelAnimMesh>);
}

const char* sStandardFragmentProgram = "#version 330 core\n\
\n\
in vec2 vTexCoord0;\n\
in vec4 vColor0;\n\
uniform sampler2D texture0;\n\
uniform float alphaTestF;\n\
out vec4 Color;\n\
\n\
void main()\n\
{\n\
Color = texture(texture0, vTexCoord0);\n\
if (Color.a > alphaTestF) discard;\n\
Color.r = Color.r * vColor0.r * vColor0.a;\n\
Color.g = Color.g * vColor0.g * vColor0.a;\n\
Color.b = Color.b * vColor0.b * vColor0.a;\n\
}\n\
";

const char* sStandardVertexProgram = "#version 330 core\n\
\n\
layout(location = 0) in vec3 aPosition;\n\
layout(location = 1) in vec3 aNormal;\n\
layout(location = 2) in vec2 aTexCoord0;\n\
\n\
uniform mat4 worldMatrixProjection;\n\
uniform mat4 worldMatrix;\n\
uniform vec3 lightPos;\n\
uniform vec3 lightColor;\n\
\n\
out vec2 vTexCoord0;\n\
out vec4 vColor0;\n\
\n\
void main()\n\
{\n\
vec3 normal, lightDir;\n\
vec4 diffuse;\n\
float NdotL;\n\
\n\
normal = normalize(mat3(worldMatrix) * aNormal);\n\
\n\
lightDir = normalize(vec3(lightPos));\n\
\n\
NdotL = max(dot(normal, lightDir), 0.0);\n\
\n\
diffuse = vec4(lightColor, 1.0);\n\
\n\
gl_Position = worldMatrixProjection * vec4(aPosition,1);\n\
vTexCoord0 = aTexCoord0;\n\
vColor0 = NdotL * diffuse;\n\
vColor0.a = 1.0;\n\
}\n\
";

const char* sLineFragmentProgram = "#version 330 core\n\
\n\
in vec4 vColor0;\n\
out vec4 Color;\n\
\n\
void main()\n\
{\n\
Color = vColor0;\n\
}\n\
";

const char* sLineVertexProgram = "#version 330 core\n\
\n\
in vec3 aPosition;\n\
in vec3 aNormal;\n\
in vec4 aColor0;\n\
in vec3 aNext;\n\
\n\
uniform mat4 projection;\n\
uniform mat4 worldMatrix;\n\
uniform float lineWidth;\n\
uniform vec2 viewportScale;\n\
\n\
out vec4 vColor0;\n\
\n\
void main()\n\
{\n\
vec4 startPos = worldMatrix * vec4(aPosition, 1);\n\
vec4 endPos = worldMatrix * vec4(aNext, 1);\n\
vec4 projStartPos = projection * startPos;\n\
vec4 projEndPos = projection * endPos;\n\
vec4 dp = projEndPos - projStartPos;\n\
vec4 delta = normalize(vec4(dp.x, dp.y, 0, 0));\n\
delta = vec4(-delta.y, delta.x, 0, 0);\n\
vec4 realDelta = vec4(0,0,0,0);\n\
realDelta += delta * aNormal.x;\n\
realDelta = realDelta * lineWidth;\n\
gl_Position = ((projection * worldMatrix) * (vec4(aPosition, 1)));\n\
gl_Position.xyz /= gl_Position.w;\n\
gl_Position += vec4((realDelta.xy * viewportScale), 0, 0);\n\
gl_Position.w = gl_Position.z = 1;\n\
vColor0 = aColor0;\n\
}\n\
";

class ShapeViewer
{
public:
   struct RuntimeMeshInfo
   {
      std::vector<CelAnimMesh::Prim> mPrims;
      CelAnimMesh* mMesh;
      uint32_t mRealVertsPerFrame;
      uint32_t mRealTexVertsPerFrame;
      
      RuntimeMeshInfo() {;}
      ~RuntimeMeshInfo() {;}
   };
   
   struct RuntimeObjectInfo
   {
      uint32_t mFrame;
      uint32_t mTexFrame;
      bool mDraw;
      int32_t mLastKeyframe;
      
      RuntimeObjectInfo() : mFrame(0), mTexFrame(0), mLastKeyframe(-1), mDraw(true) {;}
      ~RuntimeObjectInfo() {;}
   };
   
   struct RuntimeDetailInfo
   {
      uint32_t startRenderObject;
      uint32_t numRenderObjects;
      
      RuntimeDetailInfo() {;}
      RuntimeDetailInfo(uint32_t so, uint32_t nro) : startRenderObject(so), numRenderObjects(nro) {;}
   };
   
   enum ShaderAttribs
   {
      kVertexAttrib_Position,
      kVertexAttrib_Position_Normals,
      kVertexAttrib_Position_TexCoords,
      kVertexAttrib_Position_Color,
      kVertexAttrib_Position_Next,
      kVertexAttrib_COUNT
   };
   
   enum ShaderUniforms
   {
      kUniform_MVPMatrix,
      kUniform_MVMatrix,
      kUniform_LightPos,
      kUniform_LightColor,
      kUniform_AlphaTestVal,
      kUniform_SamplerID,
      kUniform_COUNT
   };
   
   enum LineShaderUniforms
   {
      kLineUniform_PMatrix,
      kLineUniform_MVMatrix,
      kLineUniform_Width,
      kLineUniform_ViewportScale,
      kLineUniform_COUNT
   };
   
   struct ProgramInfo
   {
      GLuint programID;
      GLint uniformLocations[kUniform_COUNT];
      ProgramInfo() : programID(0) { memset(uniformLocations, '\0', sizeof(uniformLocations)); }
   };
   
   struct LineProgramInfo
   {
      GLuint programID;
      GLint uniformLocations[kLineUniform_COUNT];
      LineProgramInfo() : programID(0) { memset(uniformLocations, '\0', sizeof(uniformLocations)); }
   };
   
   const uint32_t kLineVertSize = (sizeof(slm::vec3) * 3) + sizeof(slm::vec4);
   
   struct ShapeThread
   {
      enum State
      {
         STOPPED,
         PLAYING,
         PLAYING_TRANSITION_WAIT,
         TRANSITIONING,
      };
      int32_t sequenceIdx;
      int32_t transitionIdx;
      uint32_t startSubsequence;
      float pos;
      
      State state;
      bool enabled;
      
      ShapeThread() : sequenceIdx(-1), transitionIdx(-1), startSubsequence(0), pos(0), enabled(true) {;}
   };
   
   std::vector<ShapeThread> mThreads;
   std::vector<int16_t> mThreadSubsequences; // Subsequence tracks for nodes + objects
   
   Shape* mShape;
   ResManager* mResourceManager;
   Palette* mPalette;
   
   bool initVB;
   
   struct _LineVert
   {
      slm::vec3 pos;
      slm::vec3 nextPos;
      slm::vec3 normal;
      slm::vec4 color;
   };
   
   GLuint mVertexBuffer;
   GLuint mTexVertexBuffer;
   GLuint mPrimitiveBuffer;
   ProgramInfo mProgram;
   LineProgramInfo mLineProgram;
   
   GLuint mLineVertexBuffer;
   
   slm::mat4 mProjectionMatrix;
   slm::mat4 mModelMatrix;
   slm::mat4 mViewMatrix;
   
   slm::vec4 mLightColor;
   slm::vec3 mLightPos;
   
   std::vector<slm::mat4> mNodeTransforms; // Current transform list
   std::vector<slm::quat> mActiveRotations; // non-gl xfms
   std::vector<slm::vec4> mActiveTranslations; // non-gl xfms
   std::vector<uint8_t> mNodeVisiblity;
   std::vector<RuntimeMeshInfo*> mRuntimeMeshInfos;
   std::vector<RuntimeObjectInfo*> mRuntimeObjectInfos;
   
   std::vector<RuntimeDetailInfo> mRuntimeDetails;
   std::vector<uint32_t> mObjectRenderID;
   
   int32_t mDefaultMaterials;
   int32_t mAlwaysNode;
   int32_t mCurrentDetail;
   
   struct LoadedTexture
   {
      GLuint texID;
      uint32_t bmpFlags;
      
      LoadedTexture() {;}
      LoadedTexture(GLuint tid, uint32_t bf) : texID(tid), bmpFlags(bf) {;}
   };
   
   struct ActiveMaterial
   {
      LoadedTexture tex;
   };
   
   std::vector<ActiveMaterial> mActiveMaterials;
   std::unordered_map<std::string, LoadedTexture> mLoadedTextures;
   
   Shape::Transform& getTransform(uint32_t i)
   {
      return mShape->mTransforms[i];
   }
   
   Shape::Detail& getDetail(uint32_t i)
   {
      return mShape->mDetails[i];
   }
   
   ShapeViewer()
   {
      mPalette = NULL;
      mShape = NULL;
      mResourceManager = new ResManager();
      initVB = false;
   }
   
   ~ShapeViewer()
   {
      for (RuntimeMeshInfo* itr : mRuntimeMeshInfos) { delete itr; }
      for (RuntimeObjectInfo* itr : mRuntimeObjectInfos) { delete itr; }
      if (mPalette) delete mPalette;
      if (mShape) delete mShape;
      clearVertexBuffer();
      clearTextures();
      clearRender();
      if (mResourceManager) delete mResourceManager;
   }
   
   void clear()
   {
      clearVertexBuffer();
      clearTextures();
      
      for (RuntimeMeshInfo* itr : mRuntimeMeshInfos) { delete itr; }
      for (RuntimeObjectInfo* itr : mRuntimeObjectInfos) { delete itr; }
      mRuntimeObjectInfos.clear();
      mRuntimeMeshInfos.clear();
      mNodeTransforms.clear();
      mThreads.clear();
      mThreadSubsequences.clear();
      mActiveMaterials.clear();
      mShape = NULL;
   }
   
   void initRender()
   {
      mProgram = buildProgram();
      mLineProgram = buildLineProgram();
      mLightColor = slm::vec4(1,1,1,1);
      mLightPos = slm::vec3(0,2, 2);
      
      glGenBuffers(1, &mLineVertexBuffer);
      glBindBuffer(GL_ARRAY_BUFFER, mLineVertexBuffer);
      glBufferData(GL_ARRAY_BUFFER, 6 * kLineVertSize, NULL, GL_STREAM_DRAW);
      
      GLuint VertexArrayID;
      glGenVertexArrays(1, &VertexArrayID);
      glBindVertexArray(VertexArrayID);
   }
   
   void clearRender()
   {
      glDeleteProgram(mProgram.programID);
   }
   
   GLuint compileShader(GLuint shaderType, const char* data)
   {
      GLuint shader = glCreateShader(shaderType);
      glShaderSource(shader, 1, &data, NULL);
      glCompileShader(shader);
      
      GLint status;
      glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
      if (status == GL_FALSE)
      {
         GLint infoLogLength;
         glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLogLength);
         GLchar *strInfoLog = new GLchar[infoLogLength + 1];
         glGetShaderInfoLog(shader, infoLogLength, NULL, strInfoLog);
         fprintf(stderr, "Failed to compile shader(%i)...\n%s", shaderType, strInfoLog);
         delete[] strInfoLog;
         return 0;
      }
      
      return shader;
   }
   
   LineProgramInfo buildLineProgram()
   {
      LineProgramInfo ret;
      GLuint shaders[2];
      
      shaders[0] = compileShader(GL_VERTEX_SHADER, sLineVertexProgram);
      shaders[1] = compileShader(GL_FRAGMENT_SHADER, sLineFragmentProgram);
      
      ret.programID = glCreateProgram();
      
      glAttachShader(ret.programID, shaders[0]);
      glAttachShader(ret.programID, shaders[1]);
      
      glBindAttribLocation(ret.programID, kVertexAttrib_Position, "aPosition");
      glBindAttribLocation(ret.programID, kVertexAttrib_Position_Normals, "aNormal");
      glBindAttribLocation(ret.programID, kVertexAttrib_Position_Color, "aColor0");
      glBindAttribLocation(ret.programID, kVertexAttrib_Position_Next, "aNext");
      
      glLinkProgram(ret.programID);
      
      GLint status = GL_TRUE;
      glGetProgramiv (ret.programID, GL_LINK_STATUS, &status);
      if (status == GL_FALSE)
      {
         GLint infoLogLength = 0;
         glGetProgramiv(ret.programID, GL_INFO_LOG_LENGTH, &infoLogLength);
         
         if (infoLogLength > 0)
         {
            GLchar *strInfoLog = new GLchar[infoLogLength + 1];
            glGetProgramInfoLog(ret.programID, infoLogLength, NULL, strInfoLog);
            fprintf(stderr, "Failed to link shader...\n%s\n", strInfoLog);
            delete[] strInfoLog;
         }
         glDeleteProgram(ret.programID);
         return ret;
      }
      
      glDetachShader(ret.programID, shaders[0]);
      glDetachShader(ret.programID, shaders[1]);
      
      glUseProgram(ret.programID);
      
      ret.uniformLocations[kLineUniform_PMatrix] = glGetUniformLocation(ret.programID, "projection");
      ret.uniformLocations[kLineUniform_MVMatrix] = glGetUniformLocation(ret.programID, "worldMatrix");
      ret.uniformLocations[kLineUniform_Width] = glGetUniformLocation(ret.programID, "lineWidth");
      ret.uniformLocations[kLineUniform_ViewportScale] = glGetUniformLocation(ret.programID, "viewportScale");
      
      return ret;
   }
   
   ProgramInfo buildProgram()
   {
      ProgramInfo ret;
      GLuint shaders[2];
      
      shaders[0] = compileShader(GL_VERTEX_SHADER, sStandardVertexProgram);
      shaders[1] = compileShader(GL_FRAGMENT_SHADER, sStandardFragmentProgram);
      
      ret.programID = glCreateProgram();
      
      glAttachShader(ret.programID, shaders[0]);
      glAttachShader(ret.programID, shaders[1]);
      
      glBindAttribLocation(ret.programID, kVertexAttrib_Position, "aPosition");
      glBindAttribLocation(ret.programID, kVertexAttrib_Position_Normals, "aNormal");
      glBindAttribLocation(ret.programID, kVertexAttrib_Position_TexCoords, "aTexCoord0");
      
      glLinkProgram(ret.programID);
      
      GLint status = GL_TRUE;
      glGetProgramiv (ret.programID, GL_LINK_STATUS, &status);
      if (status == GL_FALSE)
      {
         GLint infoLogLength = 0;
         glGetProgramiv(ret.programID, GL_INFO_LOG_LENGTH, &infoLogLength);
         
         if (infoLogLength > 0)
         {
            GLchar *strInfoLog = new GLchar[infoLogLength + 1];
            glGetProgramInfoLog(ret.programID, infoLogLength, NULL, strInfoLog);
            
            fprintf(stderr, "Failed to link shader...\n%s\n", strInfoLog);
            
            delete[] strInfoLog;
         }
         glDeleteProgram(ret.programID);
         return ret;
      }
      
      glDetachShader(ret.programID, shaders[0]);
      glDetachShader(ret.programID, shaders[1]);
      
      glUseProgram(ret.programID);
      
      ret.uniformLocations[kUniform_MVPMatrix] = glGetUniformLocation(ret.programID, "worldMatrixProjection");
      ret.uniformLocations[kUniform_MVMatrix] = glGetUniformLocation(ret.programID, "worldMatrix");
      ret.uniformLocations[kUniform_LightPos] = glGetUniformLocation(ret.programID, "lightPos");
      ret.uniformLocations[kUniform_LightColor] = glGetUniformLocation(ret.programID, "lightColor");
      ret.uniformLocations[kUniform_AlphaTestVal] = glGetUniformLocation(ret.programID, "alphaTestF");
      ret.uniformLocations[kUniform_SamplerID] = glGetUniformLocation(ret.programID, "texture0");
      
      return ret;
   }
   
   // Sequence Handling
   
   inline uint32_t getSubsequenceStride()
   {
      return  (mShape->mObjects.size() + mShape->mNodes.size()) * 2;
   }
   
   uint32_t addThread()
   {
      ShapeThread thread;
      thread.startSubsequence = mThreadSubsequences.size();
      mThreads.push_back(thread);
      mThreadSubsequences.resize(mThreadSubsequences.size() + getSubsequenceStride());
      for (uint32_t i=thread.startSubsequence; i<mThreadSubsequences.size(); i++)
      {
         mThreadSubsequences[i] = -1;
      }
      
      return mThreads.size()-1;
   }
   
   void setThreadSequence(uint32_t idx, int32_t sequenceId)
   {
      ShapeThread &thread = mThreads[idx];
      thread.sequenceIdx = sequenceId;
      thread.transitionIdx = -1;
      thread.pos = 0.0f;
      thread.state = sequenceId < 0 ? ShapeThread::STOPPED : ShapeThread::PLAYING;
      // Scan through nodes and objects and set subsequence track
      memset(&mThreadSubsequences[thread.startSubsequence], '\0', sizeof(uint16_t)*getSubsequenceStride());
      
      for (int k=0, sz = mShape->mNodes.size(); k<sz; k++)
      {
         Shape::Node *itr = &mShape->mNodes[k];
         mThreadSubsequences[thread.startSubsequence + k] = -1;
         for (int32_t i=itr->firstSubSequence, endI=itr->firstSubSequence + itr->numSubSequences; i<endI; i++)
         {
            if (mShape->mSubSequences[i].sequenceIdx == sequenceId)
            {
               mThreadSubsequences[thread.startSubsequence + k] = i;
               break;
            }
         }
      }
      
      uint32_t offset = mShape->mNodes.size();
      for (int k=0, sz = mShape->mObjects.size(); k<sz; k++)
      {
         Shape::Object *itr = &mShape->mObjects[k];
         mThreadSubsequences[thread.startSubsequence + offset + k] = -1;
         for (int32_t i=itr->firstSubSequence, endI=itr->firstSubSequence + itr->numSubSequences; i<endI; i++)
         {
            if (mShape->mSubSequences[i].sequenceIdx == sequenceId)
            {
               mThreadSubsequences[thread.startSubsequence + offset + k] = i;
               break;
            }
         }
      }
      
      // Reset obj states
      for (int i=0; i<mRuntimeObjectInfos.size(); i++)
      {
         mRuntimeObjectInfos[i]->mLastKeyframe = -1;
      }
   }
   
   void removeThread(uint32_t idx)
   {
      const uint32_t numSubSeqs = getSubsequenceStride();
      ShapeThread thread = mThreads[idx];
      mThreadSubsequences.erase(mThreadSubsequences.begin() + thread.startSubsequence, mThreadSubsequences.begin() + thread.startSubsequence + numSubSeqs);
      for (uint32_t i = idx+1, sz = mThreads.size(); i<sz; i++)
      {
         mThreads[i].startSubsequence -= numSubSeqs;
      }
      mThreads.erase(mThreads.begin()+idx);
   }
   
   void advanceThreads(float dt)
   {
      for (ShapeThread &thread : mThreads)
      {
         if (thread.sequenceIdx == -1 || thread.sequenceIdx >= mShape->mSequences.size())
            continue;
         
         Shape::Sequence &sequence = mShape->mSequences[thread.sequenceIdx];
         
         switch (thread.state)
         {
            case ShapeThread::STOPPED:
               break;
            case ShapeThread::TRANSITIONING: // TODO
               break;
            case ShapeThread::PLAYING_TRANSITION_WAIT: // TODO
            case ShapeThread::PLAYING:
               thread.pos += dt / sequence.duration;
               if (thread.pos > 1.0)
               {
                  if (sequence.cyclic)
                  {
                     thread.pos -= 1.0;
                     for (int i=0; i<mRuntimeObjectInfos.size(); i++)
                     {
                        mRuntimeObjectInfos[i]->mLastKeyframe = -1;
                     }
                  }
                  else
                  {
                     thread.pos = 1.0;
                     thread.state = ShapeThread::STOPPED;
                  }
               }
               break;
         }
      }
   }
   
   void animateNodes()
   {
      if (mAlwaysNode >= 0)
      {
         animateNode(mAlwaysNode);
         animateObjects(mRuntimeDetails[0]);
      }
      
      if (mCurrentDetail >= 0)
      {
         animateNode(getDetail(mCurrentDetail).rootNode);
         animateObjects(mRuntimeDetails[mCurrentDetail+1]);
      }
   }
   
   void animateObjects(RuntimeDetailInfo& runtimeDetail)
   {
      for (uint32_t i=runtimeDetail.startRenderObject; i<runtimeDetail.startRenderObject+runtimeDetail.numRenderObjects; i++)
      {
         uint32_t objIDToRender = mObjectRenderID[i];
         Shape::Object &info = mShape->mObjects[objIDToRender];
         RuntimeObjectInfo* runtimeInfo = mRuntimeObjectInfos[objIDToRender];
         
         if (runtimeInfo->mLastKeyframe < 0)
         {
            runtimeInfo->mDraw = (info.flags & Shape::OBJECT_INVISIBLE_DEFAULT) != 0 ? false : true;
            runtimeInfo->mFrame = 0;
            runtimeInfo->mTexFrame = 0;
            runtimeInfo->mLastKeyframe = 0;
         }
         
         for (int i=0; i<mThreads.size(); i++)
         {
            Shape::Keyframe kfA;
            ShapeThread &thread = mThreads[i];
            if (thread.sequenceIdx == -1 || thread.sequenceIdx >= mShape->mSequences.size() || !thread.enabled)
               continue;
            uint32_t startSub = thread.startSubsequence;
            int16_t subSeqIdx = mThreadSubsequences[startSub + mShape->mNodes.size() + objIDToRender];
            if (subSeqIdx < 0)
               continue;
            if (mShape->mSubSequences.size() == 0)
               continue;
            
            getNearestSubsequenceKeyframe(mShape->mSequences[thread.sequenceIdx],
                                          mShape->mSubSequences[subSeqIdx],
                                          runtimeInfo->mDraw,
                                          &runtimeInfo->mLastKeyframe, thread.pos, kfA);
            
            if (kfA.matIndex & Shape::KEYFRAME_VIS_MATTERS)
               runtimeInfo->mDraw = (kfA.matIndex & Shape::KEYFRAME_VIS) != 0;
            if (kfA.matIndex & Shape::KEYFRAME_FRAME_MATTERS)
               runtimeInfo->mFrame = kfA.key;
            if (kfA.matIndex & Shape::KEYFRAME_MAT_MATTERS)
               runtimeInfo->mTexFrame = (kfA.matIndex & Shape::KEYFRAME_MAT_MASK);
         }
      }
   }
   
   void getNearestSubsequenceKeyframe(const Shape::Sequence &seq, const Shape::SubSequence &subSeq, bool lastVis, int32_t *lastKF, float pos, Shape::Keyframe &outA)
   {
      int32_t prevIDX=subSeq.firstKeyFrame-1;
      uint32_t lastFrame=0;
      uint32_t lastTexFrame=0;
      uint32_t lastMatters=0;
      
      // reset start basis if we've gone backwards
      if (*lastKF >= subSeq.firstKeyFrame)
      {
         const Shape::Keyframe &kf = mShape->mKeyframes[(*lastKF)];
         if (pos < kf.pos)
            *lastKF = subSeq.firstKeyFrame;
      }
      else
      {
         *lastKF = subSeq.firstKeyFrame;
      }
      
      for (uint32_t i=(*lastKF-subSeq.firstKeyFrame); i<subSeq.numKeyFrames; i++)
      {
         const Shape::Keyframe &kf = mShape->mKeyframes[subSeq.firstKeyFrame+i];
         if (kf.pos <= pos + 0.001f)
         {
            prevIDX = subSeq.firstKeyFrame+i;
            
            if (kf.matIndex & Shape::KEYFRAME_VIS_MATTERS)
            {
               lastMatters |= Shape::KEYFRAME_VIS_MATTERS | Shape::KEYFRAME_VIS;
            }
            if (kf.matIndex & Shape::KEYFRAME_FRAME_MATTERS)
            {
               lastFrame = (kf.key);
               lastMatters |= Shape::KEYFRAME_FRAME_MATTERS;
            }
            if (kf.matIndex & Shape::KEYFRAME_MAT_MATTERS)
            {
               lastTexFrame = (kf.matIndex & Shape::KEYFRAME_MAT_MASK);
               lastMatters |= Shape::KEYFRAME_MAT_MATTERS;
            }
         }
         else if (kf.pos >= pos - 0.001f)
         {
            break;
         }
      }
      
      outA = mShape->mKeyframes[prevIDX];
      outA.matIndex = lastTexFrame | lastMatters;
      outA.key = lastFrame;
      *lastKF = prevIDX;
   }
   
   void getSubsequenceKeyframes(const Shape::Sequence &seq, const Shape::SubSequence &subSeq, uint32_t nodeIdx, float pos, Shape::Keyframe &outA, Shape::Keyframe &outB, float &outInterpolation)
   {
      int32_t prevIDX=subSeq.firstKeyFrame-1;
      int32_t nextIDX=subSeq.firstKeyFrame+subSeq.numKeyFrames;
      for (uint32_t i=0; i<subSeq.numKeyFrames; i++)
      {
         const Shape::Keyframe &kf = mShape->mKeyframes[subSeq.firstKeyFrame+i];
         if (kf.pos <= pos + 0.001f)
         {
            prevIDX = subSeq.firstKeyFrame+i;
         }
         else if (kf.pos >= pos - 0.001f)
         {
            nextIDX = subSeq.firstKeyFrame+i;
            break;
         }
      }
      
      // Refine and determine interpolation value
      if (seq.cyclic)
      {
         float diff = 0.0f;
         if (prevIDX < subSeq.firstKeyFrame)
         {
            prevIDX = subSeq.firstKeyFrame + subSeq.numKeyFrames-1;
            diff = mShape->mKeyframes[nextIDX].pos - mShape->mKeyframes[prevIDX].pos;
            outInterpolation = (pos - mShape->mKeyframes[prevIDX].pos) / diff;
         }
         else if (nextIDX >= subSeq.firstKeyFrame+subSeq.numKeyFrames)
         {
            nextIDX = subSeq.firstKeyFrame;
            diff = (mShape->mKeyframes[nextIDX].pos + 1.0f) - mShape->mKeyframes[prevIDX].pos;
            outInterpolation = (pos - mShape->mKeyframes[prevIDX].pos) / diff;
         }
         
         if (prevIDX == nextIDX)
         {
            outInterpolation = 0.0f;
         }
         else
         {
            diff = mShape->mKeyframes[nextIDX].pos - mShape->mKeyframes[prevIDX].pos;
            if (std::fpclassify(diff) == FP_ZERO)
            {
               outInterpolation = std::fpclassify(pos - mShape->mKeyframes[prevIDX].pos) == FP_ZERO ? 0.0f : 1.0f;
            }
            else
            {
               outInterpolation = (pos - mShape->mKeyframes[prevIDX].pos) / diff;
            }
         }
      }
      else
      {
         if (prevIDX < subSeq.firstKeyFrame)
         {
            prevIDX = subSeq.firstKeyFrame;
            outInterpolation = 0.0f;
         }
         else if (nextIDX >= subSeq.firstKeyFrame+subSeq.numKeyFrames)
         {
            nextIDX = subSeq.firstKeyFrame+subSeq.numKeyFrames-1;
            outInterpolation = 1.0f;
         }
         else if (prevIDX == nextIDX)
         {
            outInterpolation = 0.0f;
         }
         else
         {
            float diff = mShape->mKeyframes[nextIDX].pos - mShape->mKeyframes[prevIDX].pos;
            outInterpolation = (diff <= 0) ? 0.0f : (pos - mShape->mKeyframes[prevIDX].pos) / diff;
         }
      }
      
      assert(prevIDX >= subSeq.firstKeyFrame && prevIDX < subSeq.firstKeyFrame + subSeq.numKeyFrames);
      assert(nextIDX >= subSeq.firstKeyFrame && nextIDX < subSeq.firstKeyFrame + subSeq.numKeyFrames);
      
      outA = mShape->mKeyframes[prevIDX];
      outB = mShape->mKeyframes[nextIDX];
   }
   
   slm::mat4 interpolateXfm(const Shape::Transform &xfmA, const Shape::Transform &xfmB, float pos)
   {
      slm::quat qa = xfmA.rot.toQuat();
      slm::quat qb = xfmB.rot.toQuat();
      
      slm::quat qc = CompatInterpolate(qa, qb, pos);
      float invPos = 1.0 - pos;
      slm::vec3 pc = slm::vec3(xfmA.pos.x * invPos + xfmB.pos.x * pos,
                               xfmA.pos.y * invPos + xfmB.pos.y * pos,
                               xfmA.pos.z * invPos + xfmB.pos.z * pos);
      
      slm::mat4 outXfm(1);
      CompatQuatSetMatrix(qc, outXfm);
      outXfm[3] = slm::vec4(pc.x, pc.y, pc.z, 1);
      
      return outXfm;
   }
   
   void animateNode(uint32_t nodeIdx)
   {
      Shape::Node &node = mShape->mNodes[nodeIdx];
      slm::quat q;
      slm::mat4 xfmLocal(1);
      
      mNodeVisiblity[nodeIdx] &= ~0x2; // clear force vis
      
      // Start with setting the default transform
      {
         Shape::Transform xfmShape = getTransform(node.defaultTransform);
         slm::quat tquat = xfmShape.rot.toQuat();
         CompatQuatSetMatrix(tquat, xfmLocal);
         xfmLocal[3] = slm::vec4(xfmShape.pos.x, xfmShape.pos.y, xfmShape.pos.z, 1);
      }
      
      // If we are currently being animated, use that track instead (additional tracks will override)
      for (int i=0; i<mThreads.size(); i++)
      {
         ShapeThread &thread = mThreads[i];
         if (thread.sequenceIdx == -1 || !thread.enabled)
            continue;
         uint32_t startSub = thread.startSubsequence;
         
         int16_t subSeqIdx = mThreadSubsequences[startSub + nodeIdx];
         if (subSeqIdx != -1)
         {
            Shape::Keyframe kfA;
            Shape::Keyframe kfB;
            float xfmInterpolation = 0.0f;
            
            assert(subSeqIdx >= mShape->mNodes[nodeIdx].firstSubSequence &&
                   (subSeqIdx < mShape->mNodes[nodeIdx].firstSubSequence + mShape->mNodes[nodeIdx].numSubSequences));
            
            getSubsequenceKeyframes(mShape->mSequences[thread.sequenceIdx], mShape->mSubSequences[subSeqIdx], nodeIdx, thread.pos, kfA, kfB, xfmInterpolation);
            
            if (kfA.matIndex & Shape::KEYFRAME_VIS_MATTERS)
            {
               if (kfA.matIndex & Shape::KEYFRAME_VIS)
                  mNodeVisiblity[nodeIdx] &= 0x2;
               else
                  mNodeVisiblity[nodeIdx] |= 0x2;
            }
            
            if (kfA.key == kfB.key)
            {
               Shape::Transform xfmShape = getTransform(kfA.key);
               CompatQuatSetMatrix(xfmShape.rot.toQuat(), xfmLocal);
               xfmLocal[3] = slm::vec4(xfmShape.pos.x, xfmShape.pos.y, xfmShape.pos.z, 1);
            }
            else
            {
               Shape::Transform xfmA = getTransform(kfA.key);
               Shape::Transform xfmB = getTransform(kfB.key);
               xfmLocal = interpolateXfm(xfmA, xfmB, xfmInterpolation);
            }
         }
      }
      
      if (node.parent >= 0)
      {
         slm::mat4 parentXfm = mNodeTransforms[node.parent];
         
         slm::mat4 newslmXfm(1);
         slm::vec4 tmpLocal = xfmLocal[3];
         slm::vec4 tmpParent = parentXfm[3];
         xfmLocal[3] = parentXfm[3] = slm::vec4(0,0,0,1);
         newslmXfm = parentXfm * xfmLocal;
         newslmXfm[3] = (parentXfm * tmpLocal) + tmpParent;
         newslmXfm[3].w = 1;
         
         mNodeTransforms[nodeIdx] = newslmXfm;
      }
      else
      {
         mNodeTransforms[nodeIdx] = xfmLocal;
      }
      
      // Recurse
      Shape::NodeChildInfo info = mShape->mNodeChildren[nodeIdx+1];
      for (int32_t i=0; i<info.numChildren; i++)
      {
         animateNode(mShape->mNodeChildIds[info.firstChild+i]);
      }
   }
   
   // Loading
   
   void loadShape(Shape& inShape)
   {
      clear();
      
      mShape = &inShape;
      mAlwaysNode = mShape->mAlwaysNode;
      if (mAlwaysNode > mShape->mNodes.size()) mAlwaysNode = -1;
      
      mCurrentDetail = 0;
      
      mNodeTransforms.resize(mShape->mNodes.size());
      mActiveRotations.resize(mShape->mNodes.size());
      mActiveTranslations.resize(mShape->mNodes.size());
      mNodeVisiblity.resize(mShape->mNodes.size());
      for (size_t i=0, sz=mNodeTransforms.size(); i<sz; i++)
      {
         mNodeTransforms[i] = slm::mat4(1);
         mNodeVisiblity[i] = 0x0; // everything invisible by default
      }
      
      setRuntimeDetailNodes(mAlwaysNode);
      
      initMaterials();
      
      // Preload vertex buffer
      initVertexBuffer();
      
      mRuntimeObjectInfos.resize(mShape->mObjects.size());
      for (int i=0; i<mShape->mObjects.size(); i++)
      {
         mRuntimeObjectInfos[i] = new RuntimeObjectInfo();
      }
         
      // Setup default pose for nodes
      animateNodes();
   }
   
   inline uint32_t getNextPow2(uint32_t a)
   {
      a--;
      a |= a >> 1;
      a |= a >> 2;
      a |= a >> 4;
      a |= a >> 8;
      a |= a >> 16;
      return a + 1;
   }
   
   void copyMipDirect(uint32_t height, uint32_t src_stride, uint32_t dest_stride, uint8_t* data, uint8_t* out_data)
   {
      for (int y=0; y<height; y++)
      {
         uint8_t *srcPixels = data + (y*src_stride);
         uint8_t *destPixels = out_data + (y*dest_stride);
         memcpy(destPixels, srcPixels, src_stride);
      }
   }
   
   void copyMipRGB(uint32_t width, uint32_t height, uint32_t pad_width, Palette::Data* pal, uint8_t* data, uint8_t* out_data)
   {
      for (int y=0; y<height; y++)
      {
         uint8_t *srcPixels = data + (y*width);
         uint8_t *destPixels = out_data + (y*pad_width);
         for (int x=0; x<width; x++)
         {
            uint8_t r,g,b;
            pal->lookupRGB(srcPixels[x], r,g,b);
            *destPixels++ = r;
            *destPixels++ = g;
            *destPixels++ = b;
         }
      }
   }
   
   void copyMipRGBA(uint32_t width, uint32_t height, uint32_t pad_width, Palette::Data* pal, uint8_t* data, uint8_t* out_data, uint32_t clamp_a)
   {
      for (int y=0; y<height; y++)
      {
         uint8_t *srcPixels = data + (y*width);
         uint8_t *destPixels = out_data + (y*pad_width);
         for (int x=0; x<width; x++)
         {
            uint8_t r,g,b,a;
            pal->lookupRGBA(srcPixels[x], r,g,b,a);
            *destPixels++ = r;
            *destPixels++ = g;
            *destPixels++ = b;
            *destPixels++ = std::min((uint32_t)a * clamp_a, (uint32_t)255);
         }
      }
   }
   
   bool loadTexture(const char *filename, LoadedTexture& outTexInfo, bool force=false)
   {
      bool genTex = true;
      std::string fname = std::string(filename);
      auto itr = mLoadedTextures.find(fname);
      if (itr != mLoadedTextures.end())
      {
         outTexInfo = itr->second;
         genTex = false;
         if (!force) return true;
      }

      // Find in resources
      MemRStream mem(0, NULL);
      if (mResourceManager->openFile(filename, mem))
      {
         Bitmap* bmp = new Bitmap();
         if (bmp->read(mem))
         {
            if (genTex) glGenTextures(1, &outTexInfo.texID);
            outTexInfo.bmpFlags = bmp->mFlags;
            
            uint32_t pow2W = getNextPow2(bmp->mWidth);
            uint32_t pow2H = getNextPow2(bmp->mHeight);
            
            if (bmp->mBitDepth == 8)
            {
               Palette::Data* pal = NULL;
               
               if (bmp->mPal)
                  pal = bmp->mPal->getPaletteByIndex(bmp->mPaletteIndex);
               else if (mPalette && mPalette->mPalettes.size() > 0)
                  pal = mPalette->getPaletteByIndex(bmp->mPaletteIndex);
               else
               {
                  delete bmp;
                  glDeleteTextures(1, &outTexInfo.texID);
                  assert(false);
                  return false;
               }
               
               uint8_t* texData;
               glBindTexture(GL_TEXTURE_2D, outTexInfo.texID);
               glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
               glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
               
               if (bmp->mFlags & Bitmap::FLAG_TRANSPARENT)
               {
                  texData = new uint8_t[pow2W*pow2H*4];
                  copyMipRGBA(bmp->mWidth, bmp->mHeight, pow2W*4, pal, bmp->mMips[0], texData, 255);
                  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pow2W, pow2H, 0, GL_BGRA, GL_UNSIGNED_BYTE, texData);
               }
               else if (bmp->mFlags & Bitmap::FLAG_TRANSLUCENT)
               {
                  texData = new uint8_t[pow2W*pow2H*4];
                  copyMipRGBA(bmp->mWidth, bmp->mHeight, pow2W*4, pal, bmp->mMips[0], texData, 1);
                  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pow2W, pow2H, 0, GL_BGRA, GL_UNSIGNED_BYTE, texData);
               }
               else
               {
                  texData = new uint8_t[pow2W*pow2H*3];
                  copyMipRGB(bmp->mWidth, bmp->mHeight, pow2W*3, pal, bmp->mMips[0], texData);
                  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, pow2W, pow2H, 0, GL_BGR, GL_UNSIGNED_BYTE, texData);
               }
               
               delete[] texData;
            }
            else if (bmp->mBitDepth == 24)
            {
               glBindTexture(GL_TEXTURE_2D, outTexInfo.texID);
               glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
               glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
               uint8_t* texData = new uint8_t[pow2W*pow2H*3];
               copyMipDirect(bmp->mHeight, bmp->getStride(bmp->mWidth), pow2W*3, bmp->mMips[0], texData);
               glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, pow2W, pow2H, 0, GL_BGR, GL_UNSIGNED_BYTE, texData);
               delete[] texData;
            }
            else
            {
               assert(false);
            }
            
            // Done
            mLoadedTextures[fname] = outTexInfo;
            delete bmp;
            return true;
         }
         delete bmp;
      }
      
      return false;
   }
   
   void initMaterials()
   {
      mActiveMaterials.clear();
      
      if (!mShape->mMaterials)
      {
         assert(false);
         return;
      }
      
      mActiveMaterials.resize(mShape->mMaterials->mMaterials.size());
      for (int i=0; i<mShape->mMaterials->mMaterials.size(); i++)
      {
         Material& mat = mShape->mMaterials->mMaterials[i];
         ActiveMaterial& amat = mActiveMaterials[i];
         loadTexture((const char*)mat.mFilename, amat.tex);
      }
   }
   
   void clearTextures()
   {
      for (auto itr: mLoadedTextures) { glDeleteTextures(1, &itr.second.texID); }
      mLoadedTextures.clear();
   }
   
   bool setPalette(const char *filename)
   {
      MemRStream mem(0, NULL);
      if (mResourceManager->openFile(filename, mem))
      {
         Palette* newPal = new Palette();
         if (newPal->read(mem))
         {
            if (mPalette) delete mPalette;
            mPalette = newPal;
            clearTextures();
            if (mShape) initMaterials();
         }
      }
      return false;
   }
   
   void initVertexBuffer()
   {
      clearVertexBuffer();
      
      for (RuntimeMeshInfo* info : mRuntimeMeshInfos) { delete info; }
      mRuntimeMeshInfos.clear();
      
      // Construct a buffer consisting of all the verts
      const uint32_t vertStride = sizeof(slm::vec3) + sizeof(slm::vec3);
      
      mRuntimeMeshInfos.reserve(mShape->mMeshes.size());
      
      std::vector<slm::vec3> bufferVerts;
      std::vector<slm::vec2> bufferTVerts;
      std::vector<CelAnimMesh::Triangle> bufferTris;
      
      std::vector<uint32_t> vertMap;
      std::vector<uint32_t> texVertMap;
      std::vector<CelAnimMesh::Triangle> meshInds;
      std::vector<CelAnimMesh::Prim> meshPrims;
      
      uint32_t vertexBufferSize = 0;
      uint32_t primBufferSize = 0;
      
      for (CelAnimMesh* mesh : mShape->mMeshes)
      {
         mesh->unpackVertStructure(vertMap, texVertMap, meshInds, meshPrims);
         mesh->mFixedFrameOffsets.resize(mesh->mFrames.size());
         
         uint32_t baseVertOffset = bufferVerts.size()/2;
         uint32_t baseIndexOffset = bufferTris.size()*3;
         
         if (mesh->mFaces.size() == 0)
         {
            RuntimeMeshInfo* info = new RuntimeMeshInfo();
            info->mMesh = NULL;
            mRuntimeMeshInfos.push_back(info);
            continue;
         }
         
         // Copy output prim data
         for (CelAnimMesh::Prim& prim : meshPrims)
         {
            prim.startVerts += baseVertOffset;
            prim.startInds += baseIndexOffset;
            prim.numVerts = vertMap.size();
         }
         
         uint32_t texVertFrames = 1;
         if (mesh->mTextureVertsPerFrame > 0)
         {
            texVertFrames = mesh->mTexVerts.size() / mesh->mTextureVertsPerFrame;
         }
         
         // Emit normal verts
         int32_t prevVert = -1;
         int32_t vertCount = 0;
         for (CelAnimMesh::Frame& frame : mesh->mFrames)
         {
            uint32_t ofs = frame.firstVert;
            uint32_t idx = &frame - &mesh->mFrames[0];
            
            if (frame.firstVert < prevVert || frame.firstVert < 0)
            {
               assert(false);
            }
            
            // Reuse previous frame
            if (frame.firstVert == prevVert)
            {
               mesh->mFixedFrameOffsets[idx] = mesh->mFixedFrameOffsets[idx-1];
               continue;
            }
            
            mesh->mFixedFrameOffsets[idx] = vertCount;
            prevVert = frame.firstVert;
            vertCount += (uint32_t)vertMap.size();
            
            slm::vec3 frameScale = frame.scale;
            slm::vec3 frameOrigin = frame.origin;
            
            for (uint32_t i=0, sz = (uint32_t)vertMap.size(); i<sz; i++)
            {
               CelAnimMesh::PackedVertex &v = mesh->mVerts[vertMap[i]+ofs];
               slm::vec3 xv(v.x * frameScale.x + frameOrigin.x, v.y * frameScale.y + frameOrigin.y, v.z * frameScale.z + frameOrigin.z);
               bufferVerts.push_back(xv);
               bufferVerts.push_back(EncodedNormalTable[v.normal]);
            }
         }
         
         for (int j=0; j<texVertFrames; j++)
         {
            uint32_t ofs = j*mesh->mTextureVertsPerFrame;
            assert(mesh->mTextureVertsPerFrame <= texVertMap.size());
            
            for (uint32_t i=0, sz = (uint32_t)texVertMap.size(); i<sz; i++)
            {
               bufferTVerts.push_back(mesh->mTexVerts[texVertMap[i]+ofs]);
            }
         }
         
         RuntimeMeshInfo* info = new RuntimeMeshInfo();
         info->mPrims = meshPrims;
         info->mMesh = mesh;
         info->mRealVertsPerFrame = (uint32_t)vertMap.size();
         info->mRealTexVertsPerFrame = (uint32_t)texVertMap.size();
         mRuntimeMeshInfos.push_back(info);
         bufferTris.insert(bufferTris.end(), meshInds.begin(), meshInds.end());
         
         // Clear offsets
         vertMap.clear();
         texVertMap.clear();
         meshInds.clear();
         meshPrims.clear();
      }
      
      vertexBufferSize = bufferVerts.size()*vertStride;
      primBufferSize = bufferTris.size()*6;
      
      if (vertexBufferSize == 0 || primBufferSize == 0)
         return;
      
      glGenBuffers(1, &mVertexBuffer);
      glGenBuffers(1, &mTexVertexBuffer);
      glGenBuffers(1, &mPrimitiveBuffer);
      
      glBindBuffer(GL_ARRAY_BUFFER, mVertexBuffer);
      glBufferData(GL_ARRAY_BUFFER, bufferVerts.size()*sizeof(slm::vec3), &bufferVerts[0], GL_STATIC_DRAW);
      
      glBindBuffer(GL_ARRAY_BUFFER, mTexVertexBuffer);
      glBufferData(GL_ARRAY_BUFFER, bufferTVerts.size()*sizeof(slm::vec2), &bufferTVerts[0], GL_STATIC_DRAW);
      
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mPrimitiveBuffer);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, bufferTris.size()*6, &bufferTris[0], GL_STATIC_DRAW);
   }
   
   void clearVertexBuffer()
   {
      if (!initVB)
         return;
      
      glDeleteBuffers(1, &mVertexBuffer);
      glDeleteBuffers(1, &mTexVertexBuffer);
      glDeleteBuffers(1, &mPrimitiveBuffer);
      initVB = false;
   }
   
   // Rendering
   
   void updateMVP()
   {
      slm::mat4 combined = mProjectionMatrix * mViewMatrix * mModelMatrix;
      glUniformMatrix4fv(mProgram.uniformLocations[kUniform_MVPMatrix], 1, GL_FALSE, combined.begin());
      combined = mViewMatrix * mModelMatrix;
      glUniformMatrix4fv(mProgram.uniformLocations[kUniform_MVMatrix], 1, GL_FALSE, combined.begin());
   }
   
   void setRuntimeDetailNodes(int32_t alwaysNodeId)
   {
      mRuntimeDetails.clear();
      mObjectRenderID.clear();
      
      if (mAlwaysNode > 0)
      {
         RuntimeDetailInfo alwaysInfo = addRuntimeDetailForNode(mAlwaysNode, mObjectRenderID);
         mRuntimeDetails.push_back(alwaysInfo);
      }
      else
      {
         mRuntimeDetails.push_back(RuntimeDetailInfo(0,0));
      }
      
      for (Shape::Detail &detail : mShape->mDetails)
      {
         mRuntimeDetails.emplace_back(addRuntimeDetailForNode(detail.rootNode, mObjectRenderID));
      }
   }
   
   RuntimeDetailInfo addRuntimeDetailForNode(int32_t nodeIdx, std::vector<uint32_t> &outList)
   {
      if (nodeIdx < 0)
         return RuntimeDetailInfo(0,0);
      
      bool* outUsedOjects = new bool[mShape->mObjects.size()];
      memset(outUsedOjects, '\0', mShape->mObjects.size());
      markNode(outUsedOjects, &mShape->mObjects[0], mShape->mObjects.size(), nodeIdx);
      
      uint32_t startObj = outList.size();
      for (int i=0; i<mShape->mObjects.size(); i++)
      {
         if (!outUsedOjects[i])
            continue;
         outList.push_back(i);
      }
      
      delete[] outUsedOjects;
      return RuntimeDetailInfo(startObj, outList.size() - startObj);
   }
   
   void markNode(bool *outObjectList, const Shape::Object* objList,  uint32_t numObjects, uint32_t nodeIdx)
   {
      for (int i=0; i<numObjects; i++)
      {
         if (!outObjectList[i])
         {
            const Shape::Object &obj = objList[i];
            if (obj.nodeIndex == nodeIdx)
               outObjectList[i] = true;
         }
      }
      
      Shape::NodeChildInfo info = mShape->mNodeChildren[nodeIdx+1];
      for (int32_t i=0; i<info.numChildren; i++)
      {
         markNode(outObjectList, objList, numObjects, mShape->mNodeChildIds[info.firstChild+i]);
      }
   }
   
   void updateNodeVisibility(uint32_t nodeIdx, bool parentVisible)
   {
      if (parentVisible)
      {
         if (mNodeVisiblity[nodeIdx] & 0x2)
            parentVisible = false;
      }
      
      if (parentVisible)
      {
         mNodeVisiblity[nodeIdx] |= 0x1;
      }
      
      Shape::NodeChildInfo info = mShape->mNodeChildren[nodeIdx+1];
      for (int32_t i=0; i<info.numChildren; i++)
      {
         uint32_t nodeIdx = mShape->mNodeChildIds[info.firstChild+i];
         updateNodeVisibility(nodeIdx, parentVisible);
      }
   }
   
   void determineNodeVisibility()
   {
      // Hide everything by default
      for (int i=0; i<mNodeVisiblity.size(); i++)
      {
         mNodeVisiblity[i] &= 0x2; // 0x2 == force invisible flag
      }
      
      if (mAlwaysNode >= 0)
      {
         mNodeVisiblity[mAlwaysNode] = 0x1;
         updateNodeVisibility(mAlwaysNode, true);
      }
      
      if (mCurrentDetail >= 0)
      {
         updateNodeVisibility(getDetail(mCurrentDetail).rootNode, true);
      }
   }
   
   void selectDetail(float dist, int w, int h)
   {
      float size;
      if (dist <= 0.0f)
      {
         size = 1000.0f;
      }
      else
      {
         size = atan(mShape->mRadius/dist);
         size *= std::max<float>(w, h) / slm::radians(90.0);
      }
      
      mCurrentDetail = 0;
      for (int i=0; i<mShape->mDetails.size(); i++)
      {
         Shape::Detail &detail = mShape->mDetails[i];
         if (size <= detail.size)
         {
            mCurrentDetail = i;
         }
      }
   }
   
   void drawLine(slm::vec3 start, slm::vec3 end, slm::vec4 color, float width)
   {
      glUseProgram(mLineProgram.programID);
      glDisable(GL_DEPTH_TEST);
      glDisable(GL_CULL_FACE);
      
      glUniformMatrix4fv(mLineProgram.uniformLocations[kLineUniform_PMatrix], 1, GL_FALSE, mProjectionMatrix.begin());
      slm::mat4 combined = mViewMatrix;// * mModelMatrix;
      glUniformMatrix4fv(mLineProgram.uniformLocations[kLineUniform_MVMatrix], 1, GL_FALSE, combined.begin());
      
      glUniform1f(mLineProgram.uniformLocations[kLineUniform_Width], width);
      glUniform2f(mLineProgram.uniformLocations[kLineUniform_ViewportScale], 1.0/640.0, 1.0/480.0);
      
      glEnableVertexAttribArray(kVertexAttrib_Position);
      glEnableVertexAttribArray(kVertexAttrib_Position_Normals);
      glDisableVertexAttribArray(kVertexAttrib_Position_TexCoords);
      glEnableVertexAttribArray(kVertexAttrib_Position_Color);
      glEnableVertexAttribArray(kVertexAttrib_Position_Next);
      
      glBindBuffer(GL_ARRAY_BUFFER, mLineVertexBuffer);
      
      // vc
      glVertexAttribPointer(kVertexAttrib_Position, 3, GL_FLOAT, GL_FALSE, kLineVertSize, NULL);
      // nvc
      glVertexAttribPointer(kVertexAttrib_Position_Next, 3, GL_FLOAT, GL_FALSE, kLineVertSize, ((uint8_t*)NULL)+sizeof(slm::vec3));
      // n
      glVertexAttribPointer(kVertexAttrib_Position_Normals, 3, GL_FLOAT, GL_FALSE, kLineVertSize, ((uint8_t*)NULL)+sizeof(slm::vec3)+sizeof(slm::vec3));
      // col
      glVertexAttribPointer(kVertexAttrib_Position_Color, 4, GL_FLOAT, GL_FALSE, kLineVertSize, ((uint8_t*)NULL)+sizeof(slm::vec3)+sizeof(slm::vec3)+sizeof(slm::vec3));
      
      _LineVert verts[6];
      verts[0].pos = start;
      verts[0].nextPos = end;
      verts[0].normal = slm::vec3(-1,0,0); // b
      verts[0].color = color;
      verts[1].pos = start;
      verts[1].nextPos = end;
      verts[1].normal = slm::vec3(1,0,0); // t
      verts[1].color = color;
      verts[2].pos = end;
      verts[2].nextPos = start;
      verts[2].normal = slm::vec3(1,0,0); // t
      verts[2].color = color;
      
      verts[3].pos = end;
      verts[3].nextPos = start;
      verts[3].normal = slm::vec3(1,0,0); // t
      verts[3].color = color;
      verts[4].pos = end;
      verts[4].nextPos = start;
      verts[4].normal = slm::vec3(-1,0,0); // b
      verts[4].color = color;
      verts[5].pos = start;
      verts[5].nextPos = end;
      verts[5].normal = slm::vec3(1,0,0); // b
      verts[5].color = color;
      
      glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
      glDrawArrays(GL_TRIANGLES, 0, 6);
   }
   
   void render()
   {
      determineNodeVisibility();
      
      glUseProgram(mProgram.programID);
      glUniform3fv(mProgram.uniformLocations[kUniform_LightColor], 1, mLightColor.begin());
      glUniform3fv(mProgram.uniformLocations[kUniform_LightPos], 1, mLightPos.begin());
      glUniform1i(mProgram.uniformLocations[kUniform_SamplerID], 0);
      glUniform1f(mProgram.uniformLocations[kUniform_AlphaTestVal], 1.1f);
      
      glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
      glEnableVertexAttribArray(kVertexAttrib_Position);
      glEnableVertexAttribArray(kVertexAttrib_Position_Normals);
      glEnableVertexAttribArray(kVertexAttrib_Position_TexCoords);
      glDisableVertexAttribArray(kVertexAttrib_Position_Color);
      glDisableVertexAttribArray(kVertexAttrib_Position_Next);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mPrimitiveBuffer);
      glEnable(GL_CULL_FACE);
      glFrontFace(GL_CW);
      glEnable(GL_DEPTH_TEST);
      glDepthFunc(GL_LESS);
      
      if (mAlwaysNode > 0)
      {
         renderObjects(mRuntimeDetails[0]);
      }
      
      if (mCurrentDetail < 0)
         return;
      
      renderObjects(mRuntimeDetails[mCurrentDetail+1]);
   }
   
   void renderObjects(RuntimeDetailInfo& runtimeDetail)
   {
      const uint32_t vertStride = sizeof(slm::vec3) + sizeof(slm::vec3);
      slm::mat4 firstXfm = slm::inverse(mNodeTransforms[0]);
      slm::mat4 baseModel = mModelMatrix;
      slm::mat4 y_up = slm::rotation_x(slm::radians(-90.0f));
      
      for (uint32_t i=runtimeDetail.startRenderObject; i<runtimeDetail.startRenderObject+runtimeDetail.numRenderObjects; i++)
      {
         uint32_t objIDToRender = mObjectRenderID[i];
         Shape::Object &info = mShape->mObjects[objIDToRender];
         if (info.meshIndex == -1)
            continue;
         
         RuntimeObjectInfo* runtimeInfo = mRuntimeObjectInfos[objIDToRender];
         RuntimeMeshInfo* runtimeMeshInfo = mRuntimeMeshInfos[info.meshIndex];
         CelAnimMesh* mesh = runtimeMeshInfo->mMesh;
         
         if (mesh == NULL || !runtimeInfo->mDraw)
            continue;
         
         if (info.nodeIndex >= 0 && ((mNodeVisiblity[info.nodeIndex] & 0x1) == 0))
            continue;
         
         if (info.nodeIndex < 0)
            continue;
         
         if (runtimeInfo->mFrame >= mesh->mFrames.size())
         {
            printf("Mesh frame invalid (%i), objID %i.\n", runtimeInfo->mFrame, objIDToRender);
            runtimeInfo->mFrame= 0;
         }
         
         //assert(info.offset.x == 0);
         
         slm::mat4 slmMat = mNodeTransforms[info.nodeIndex];
         
         slmMat[3] = slm::vec4(0,0,0,1);
         //slmMat = slm::transpose(slmMat);
         slmMat[3] = mNodeTransforms[info.nodeIndex][3];
         
         assert(slmMat[3].w == 1);
         
         mModelMatrix = baseModel * y_up * firstXfm * slmMat * slm::translation(info.offset);
         updateMVP();
         
         uint32_t ofsVerts = mesh->mFixedFrameOffsets[runtimeInfo->mFrame];
         uint32_t ofsTexVerts = runtimeMeshInfo->mRealTexVertsPerFrame * runtimeInfo->mTexFrame * sizeof(slm::vec2);
         
         glBindBuffer(GL_ARRAY_BUFFER, mVertexBuffer);
         glVertexAttribPointer(kVertexAttrib_Position, 3, GL_FLOAT, GL_FALSE, vertStride, NULL);
         glVertexAttribPointer(kVertexAttrib_Position_Normals, 3, GL_FLOAT, GL_FALSE, vertStride, ((uint8_t*)NULL)+sizeof(slm::vec3));
         glBindBuffer(GL_ARRAY_BUFFER, mTexVertexBuffer);
         glVertexAttribPointer(kVertexAttrib_Position_TexCoords, 2, GL_FLOAT, GL_FALSE, sizeof(slm::vec2), ((uint8_t*)NULL)+ofsTexVerts);
         
         for (CelAnimMesh::Prim& prim: runtimeMeshInfo->mPrims)
         {
            int32_t matIdx = prim.mat;
            if (matIdx < 0)
               continue;
            
            if (matIdx > mActiveMaterials.size())
               matIdx = 0;
            
            glBindTexture(GL_TEXTURE_2D, mActiveMaterials[matIdx].tex.texID);
            glDisable(GL_BLEND);
            
            if (mActiveMaterials[matIdx].tex.bmpFlags &Bitmap::FLAG_TRANSPARENT)
            {
               glUniform1f(mProgram.uniformLocations[kUniform_AlphaTestVal], 0.65f);
            }
            
            if (mActiveMaterials[matIdx].tex.bmpFlags & (Bitmap::FLAG_TRANSLUCENT | Bitmap::FLAG_ADDITIVE | Bitmap::FLAG_SUBTRACTIVE))
            {
               glEnable(GL_BLEND);
               glBlendEquation(GL_FUNC_ADD);
               if (mActiveMaterials[matIdx].tex.bmpFlags & Bitmap::FLAG_ADDITIVE)
               {
                  glBlendFunc(GL_SRC_ALPHA, GL_ONE);
               }
               else if (mActiveMaterials[matIdx].tex.bmpFlags & Bitmap::FLAG_SUBTRACTIVE)
               {
                  glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
               }
               else
               {
                  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
               }
            }
            else
            {
               glBlendFunc(GL_ONE, GL_ZERO);
            }
            
            glDrawRangeElementsBaseVertex(GL_TRIANGLES, 0, prim.numVerts, prim.numInds, GL_UNSIGNED_SHORT, ((uint16_t*)NULL) + prim.startInds, prim.startVerts + ofsVerts);
            
            if (mActiveMaterials[matIdx].tex.bmpFlags &Bitmap::FLAG_TRANSPARENT)
            {
               glUniform1f(mProgram.uniformLocations[kUniform_AlphaTestVal], 1.1f);
            }
         }
      }
      
      mModelMatrix = baseModel;
   }
   
   void renderNodes(int32_t nodeIdx, slm::vec3 parentPos, int32_t highlightIdx)
   {
      if (nodeIdx < 0)
         return;
      
      slm::mat4 firstXfm = slm::inverse(mNodeTransforms[0]);
      slm::mat4 baseModel = mModelMatrix;
      slm::mat4 y_up = slm::rotation_x(slm::radians(-90.0f));
      
      slm::mat4 slmMat = mNodeTransforms[nodeIdx];
      
      slmMat[3] = slm::vec4(0,0,0,1);
      //slmMat = slm::transpose(slmMat);
      slmMat[3] = mNodeTransforms[nodeIdx][3];
      
      assert(slmMat[3].w == 1);
      
      slm::vec4 pos = baseModel * y_up * firstXfm * slmMat * slm::vec4(0,0,0,1);
      
      drawLine(pos.xyz(), parentPos, nodeIdx == highlightIdx ? slm::vec4(0,1,0,1) : slm::vec4(1,0,0,1), 1);
      
      // Recurse
      Shape::NodeChildInfo info = mShape->mNodeChildren[nodeIdx+1];
      for (int32_t i=0; i<info.numChildren; i++)
      {
         renderNodes(mShape->mNodeChildIds[info.firstChild+i], pos.xyz(), highlightIdx);
      }
   }
};

class ShapeViewerController
{
public:
   slm::vec3 mViewPos;
   ShapeViewer mViewer;
   SDL_Window* mWindow;
   float xRot, yRot, mDetailDist;
   Shape* mShape;
   int32_t mHighlightNodeIdx;
   std::string mPaletteName;
   
   std::vector<const char*> mSequenceList;
   std::vector<int> mNextSequence;
   
   int32_t mRemoveThreadId;
   bool mRenderNodes;
   bool mManualThreads;
   
   ShapeViewerController(SDL_Window* window)
   {
      mViewPos = slm::vec3(0,0,0);
      mViewer.initRender();
      mWindow = window;
      xRot = mDetailDist = 0;
      yRot = slm::radians(180.0f);
      mShape = NULL;
      mHighlightNodeIdx = -1;
      mRemoveThreadId = -1;
      mPaletteName = "ice.day.ppl";
      mRenderNodes = true;
      mManualThreads = false;
   }
   
   ~ShapeViewerController()
   {
      if (mShape)
         delete mShape;
   }
   
   void updateNextSequence()
   {
      mNextSequence.resize(mViewer.mThreads.size());
      for (int i=0; i<mViewer.mThreads.size(); i++)
      {
         mNextSequence[i] = mViewer.mThreads[i].sequenceIdx;
      }
   }
   
   void loadShape(const char *filename, int pathIdx=-1)
   {
      MemRStream rStream(0, NULL);
      mViewer.clear();
      if (mShape)
         delete mShape;
      mShape = NULL;
      
      if (mViewer.mResourceManager->openFile(filename, rStream, pathIdx))
      {
         DarkstarPersistObject* obj = DarkstarPersistObject::createFromStream(rStream);
         if (obj)
         {
            mShape = ((Shape*)obj);
            mViewer.clear();
            mViewer.setPalette(mPaletteName.c_str());
            mViewer.loadShape(*mShape);
            
            uint32_t thr = mViewer.addThread();
            mViewer.setThreadSequence(thr, 0);
            
            mViewPos = slm::vec3(0, mViewer.mShape->mCenter.z, mViewer.mShape->mRadius);
            
            mSequenceList.resize(mShape->mSequences.size());
            updateNextSequence();
            
            for (int i=0; i<mViewer.mShape->mSequences.size(); i++)
            {
               mSequenceList[i] = mShape->getName(mShape->mSequences[i].name);
            }
         }
      }
   }
   
   void update(float dt)
   {
      glClearColor(0,1,1,1);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      
      mViewer.mModelMatrix = slm::rotation_x(xRot) * slm::rotation_y(yRot);
      mViewer.mViewMatrix = slm::mat4(1) * slm::translation(-mViewPos);
      
      int w, h;
      SDL_GetWindowSize(mWindow, &w, &h);
      mViewer.mProjectionMatrix = slm::perspective_fov_rh( slm::radians(90.0), (float)w/(float)h, 0.01f, 10000.0f);
      
      if (!mManualThreads)
         mViewer.advanceThreads(dt);
      mViewer.selectDetail(mDetailDist, w, h);
      mViewer.animateNodes();
      mViewer.render();
      if (mRenderNodes)
         mViewer.renderNodes(mShape->mDetails[mViewer.mCurrentDetail].rootNode, slm::vec3(0,0,0), mHighlightNodeIdx);
      
      // Now render gui
      
      ImGui::Begin("Nodes");
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.75);
      nodeTree(0);
      ImGui::PopStyleVar(1);
      ImGui::End();
      
      ImGui::Begin("Anim");
      char buffer[1024];
      
      if (ImGui::Button("Add Thread"))
      {
         mViewer.addThread();
         updateNextSequence();
      }
      
      ImGui::SameLine();
      ImGui::Checkbox("Manual Control", &mManualThreads);
      
      if (mRemoveThreadId >= 0)
      {
         mViewer.removeThread(mRemoveThreadId);
         mRemoveThreadId = -1;
      }
      
      for (ShapeViewer::ShapeThread &thread : mViewer.mThreads)
      {
         int32_t idx = &thread - &mViewer.mThreads[0];
         snprintf(buffer, 1024, "Thread %i", idx);
         
         bool vis = ImGui::CollapsingHeader(buffer);
         ImGui::SameLine();
         
         if (thread.sequenceIdx == -1 || thread.sequenceIdx >= mViewer.mShape->mSequences.size())
         {
            snprintf(buffer, 1024, "INVALID");
         }
         else
         {
            snprintf(buffer, 1024, "seq=%s pos=%f",
                     thread.sequenceIdx == -1 ? "NULL" : mShape->getName(mShape->mSequences[thread.sequenceIdx].name),
                     thread.pos);
         }
         
         ImGui::Text(buffer);
         
         if (vis)
         {
            snprintf(buffer, 1024, "Enabled##th%i", idx);
            ImGui::Checkbox(buffer, &mViewer.mThreads[idx].enabled);
            ImGui::SameLine();
            snprintf(buffer, 1024, "Remove##th%i", idx);
            if (ImGui::Button(buffer))
               mRemoveThreadId = idx;
            snprintf(buffer, 1024, "Pos##th%i", idx);
            ImGui::SliderFloat(buffer, &mViewer.mThreads[idx].pos, 0.0f, 1.0f);
            ImGui::NewLine();
            snprintf(buffer, 1024, "Sequences##th%i", idx);
            ImGui::ListBox(buffer, &mNextSequence[idx], &mSequenceList[0], mShape->mSequences.size());
         }
      }
      
      ImGui::End();
      
      ImGui::Begin("View");
      ImGui::SliderAngle("X Rotation", &xRot);
      ImGui::SliderAngle("Y Rotation", &yRot);
      ImGui::SliderFloat("Detail Distance", &mDetailDist, 0, 1000.0f);
      ImGui::Checkbox("Render Nodes", &mRenderNodes);
      ImGui::End();
      
      
      // Update state changed by gui
      for (int i=0; i<mNextSequence.size(); i++)
      {
         if (mNextSequence[i] != mViewer.mThreads[i].sequenceIdx)
         {
            mViewer.setThreadSequence(i, mNextSequence[i]);
         }
      }
   }
   
   void nodeTree(int32_t nodeIdx)
   {
      if (nodeIdx < 0)
         return;
      
      Shape::NodeChildInfo info = mShape->mNodeChildren[nodeIdx+1];
      uint32_t baseFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
      
      bool visDetail = (nodeIdx == mShape->mDetails[mViewer.mCurrentDetail].rootNode);
      
      if (visDetail)
      {
         ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0);
      }
      
      bool vis = ImGui::TreeNodeEx(mShape->getName(mShape->mNodes[nodeIdx].name), info.numChildren > 0 ? baseFlags : baseFlags|ImGuiTreeNodeFlags_Leaf);
      if (ImGui::IsItemClicked())
      {
         mHighlightNodeIdx = nodeIdx;
      }
                          
      if (vis)
      {
         for (int32_t i=0; i<info.numChildren; i++)
         {
            nodeTree(mShape->mNodeChildIds[info.firstChild+i]);
         }
         ImGui::TreePop();
      }
      
      if (visDetail)
      {
         ImGui::PopStyleVar(1);
      }
   }
   
};

int main(int argc, const char * argv[])
{
   SDL_Window* window;
   const uint32_t tickMS = 1000.0 / 60;
   
   assert(sizeof(slm::vec2) == 8);
   assert(sizeof(slm::vec3) == 12);
   assert(sizeof(slm::vec4) == 16);
   
   DarkstarPersistObject::initStatics();
   
   if (SDL_Init(SDL_INIT_EVERYTHING) < 0) {
      printf("Couldn't initialize SDL: %s\n", SDL_GetError());
      return (1);
   }
   
   window = SDL_CreateWindow( "DTS Viewer", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 800, 600, SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE );
   if( window == NULL ) {
      printf( "Window could not be created! SDL_Error: %s\n", SDL_GetError() );
      return (1);
   }
   
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
   SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
   SDL_GL_SetSwapInterval(1);
   
   SDL_GLContext mainContext = SDL_GL_CreateContext(window);
   
   if (gl3wInit()) {
      fprintf(stderr, "failed to initialize OpenGL\n");
      return -1;
   }
   
   ShapeViewerController controller(window);
   
   for (int i=1; i<argc; i++)
   {
      const char *path = argv[i];
      if (path && path[0] == '-')
         break;
      std::string ext = fs::extension(path);
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      
      if (ext == ".dts")
         controller.loadShape(path);
      else if (ext == ".vol")
         controller.mViewer.mResourceManager->addVolume(path);
      else if (ext == ".ppl" || ext == ".pal")
         controller.mPaletteName = path;
      else if (ext == "")
         controller.mViewer.mResourceManager->mPaths.emplace_back(path);
   }
   
   if (!controller.mShape)
   {
      fprintf(stderr, "please specify a starting shape to load\n");
      return -1;
   }
   
   // Init gui
   IMGUI_CHECKVERSION();
   ImGui::CreateContext();
   ImGuiIO& io = ImGui::GetIO(); (void)io;
   
   ImGui_ImplSDL2_InitForOpenGL(window, &mainContext);
   ImGui_ImplOpenGL3_Init("#version 150");
   ImGui::StyleColorsDark();
   
   SDL_Event event;
   bool running = true;
   
   slm::vec3 deltaMovement(0);
   uint32_t lastTicks = SDL_GetTicks();
   
   int selectedFileIdx = -1;
   int selectedVolumeIdx = -1;
   std::vector<ResManager::EnumEntry> fileList;
   std::string shapeExt = ".dts";
   controller.mViewer.mResourceManager->enumerateFiles(fileList, selectedVolumeIdx, &shapeExt);
   std::vector<const char*> cFileList;
   std::vector<std::string> sFileList;
   std::vector<const char*> cVolumeList;
   sFileList.resize(fileList.size());
   for (int i=0; i<fileList.size(); i++)
   {
      sFileList[i] = fileList[i].filename;
   }
   for (int i=0; i<fileList.size(); i++)
   {
      cFileList.push_back(sFileList[i].c_str());
   }
   controller.mViewer.mResourceManager->enumerateSearchPaths(cVolumeList);
   
   int oldSelectedVolumeIdx = -1;
   int oldSelectedFileIdx = -1;
   
   while (running)
   {
      uint32_t curTicks = SDL_GetTicks();
      float dt = ((float)(curTicks - lastTicks)) / 1000.0f;
      lastTicks = curTicks;
      controller.mViewPos += deltaMovement * dt;
      
      int w, h;
      SDL_GetWindowSize(window, &w, &h);
      
      glViewport(0,0,w,h);
      
      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplSDL2_NewFrame(window);
      ImGui::NewFrame();
      
      if (oldSelectedVolumeIdx != selectedVolumeIdx)
      {
         fileList.clear();
         controller.mViewer.mResourceManager->enumerateFiles(fileList, selectedVolumeIdx, &shapeExt);
         oldSelectedVolumeIdx = selectedVolumeIdx;
         
         cFileList.clear();
         sFileList.resize(fileList.size());
         for (int i=0; i<fileList.size(); i++)
         {
            sFileList[i] = fileList[i].filename;
         }
         for (int i=0; i<fileList.size(); i++)
         {
            cFileList.push_back(sFileList[i].c_str());
         }
         
         oldSelectedFileIdx = selectedFileIdx = -1;
      }
      
      if (oldSelectedFileIdx != selectedFileIdx)
      {
         controller.loadShape(cFileList[selectedFileIdx], selectedVolumeIdx);
         oldSelectedFileIdx = selectedFileIdx;
      }
      
      while (SDL_PollEvent(&event))
      {
         switch (event.type)
         {
            case SDL_KEYDOWN:
            case SDL_KEYUP:
            {
               switch (event.key.keysym.sym)
               {
                  case SDLK_LEFT:  deltaMovement.x = event.type == SDL_KEYDOWN ? -1 : 0; break;
                  case SDLK_RIGHT: deltaMovement.x = event.type == SDL_KEYDOWN ? 1 : 0; break;
                  case SDLK_UP:    deltaMovement.y = event.type == SDL_KEYDOWN ? 1 : 0; break;
                  case SDLK_DOWN:  deltaMovement.y = event.type == SDL_KEYDOWN ? -1 : 0; break;
                  case SDLK_q:  deltaMovement.z = event.type == SDL_KEYDOWN ? -1 : 0; break;
                  case SDLK_e:  deltaMovement.z = event.type == SDL_KEYDOWN ? 1 : 0; break;
               }
            }
               break;
               
            case SDL_QUIT:
               running = false;
               break;
         }
      }
      
      controller.update(dt);
      
      ImGui::Begin("Browse");
      ImGui::Columns(2);
      ImGui::ListBox("##bvols", &selectedVolumeIdx, &cVolumeList[0], cVolumeList.size());
      ImGui::NextColumn();
      ImGui::ListBox("##bfiles", &selectedFileIdx, &cFileList[0], cFileList.size());
      
      ImGui::End();
      
      ImGui::Render();
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      SDL_GL_SwapWindow(window);
      
      uint32_t endTicks = SDL_GetTicks();
      if (endTicks - lastTicks < tickMS)
      {
         SDL_Delay(tickMS - (endTicks - lastTicks));
      }
   }
   
   SDL_DestroyWindow( window );
   SDL_Quit();

   return 0;
}
