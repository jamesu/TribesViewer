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

#ifndef _COMMONDATA_H_
#define _COMMONDATA_H_

#include <algorithm>
#include <functional>

struct _LineVert
{
   slm::vec3 pos;
   slm::vec3 nextPos;
   slm::vec3 normal;
   slm::vec4 color;
};

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
   
   inline bool readSString32(std::string &outS)
   {
      uint32_t size;
      if (!read(size)) return false;
      
      char *str = new char[size+1];
      if (read(size, str))
      {
         str[size] = '\0';
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
   
   // WRITE
   
   // For array types
   template<class T, int N> inline bool write( T (&value)[N] )
   {
      if (mPos >= mSize || mPos+(sizeof(T)*N) > mSize)
         return false;
      
      memcpy(mPtr+mPos, &value, sizeof(T)*N);
      mPos += sizeof(T)*N;
      
      return true;
   }
   
   // For normal scalar types
   template<typename T> inline bool write(T &value)
   {
      T* tptr = (T*)(mPtr+mPos);
      if (mPos >= mSize || mPos+sizeof(T) > mSize)
         return false;
      
      *tptr = value;
      mPos += sizeof(T);
      
      return true;
   }
   
   inline bool write(uint32_t size, void* data)
   {
      if (mPos >= mSize || mPos+size > mSize)
         return false;
      
      memcpy(mPtr+mPos, data, size);
      mPos += size;
      
      return true;
   }
   
   inline bool writeSString(std::string &outS)
   {
      uint16_t size = (uint16_t)outS.size();
      if (!write(size)) return false;
      
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
   
   Palette();
   ~Palette();
   
   bool readMSPAL(MemRStream& mem);
   
   uint32_t calcLookupSize(uint32_t type);
   
   bool read(MemRStream& mem);
   
   Data* getPaletteByIndex(uint32_t idx);
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
   bool mBGR;
   
   Bitmap();
   ~Bitmap();
   
   bool readMSBMP(MemRStream& mem);
   
   inline uint32_t getStride(uint32_t width) const { return 4 * ((width * mBitDepth + 31)/32); }
   
   inline uint8_t* getAddress(uint32_t mip, uint32_t x, uint32_t y)
   {
      assert(mip == 0);
      uint32_t stride = getStride(mWidth);
      return mMips[mip] + ((stride * y) + ((mBitDepth * x) / 8));
   }
   
   bool read(MemRStream& mem);
};


inline void copyMipDirect(uint32_t height, uint32_t src_stride, uint32_t dest_stride, uint8_t* data, uint8_t* out_data)
{
   for (int y=0; y<height; y++)
   {
      uint8_t *srcPixels = data + (y*src_stride);
      uint8_t *destPixels = out_data + (y*dest_stride);
      memcpy(destPixels, srcPixels, src_stride);
   }
}

inline void copyMipDirectPadded2(uint32_t height, uint32_t src_stride, uint32_t dest_stride, uint8_t* data, uint8_t* out_data)
{
   for (int y=0; y<height; y++)
   {
      uint8_t *srcPixels = data + (y*src_stride);
      uint8_t *destPixels = out_data + (y*dest_stride);
      for (int x=0; x<src_stride; x+=2)
      {
         *destPixels++ = *srcPixels++;
         *destPixels++ = *srcPixels++;
      }
   }
}

inline void copyMipDirectPadded(uint32_t height, uint32_t src_stride, uint32_t dest_stride, uint8_t* data, uint8_t* out_data)
{
   for (int y=0; y<height; y++)
   {
      uint8_t *srcPixels = data + (y*src_stride);
      uint8_t *destPixels = out_data + (y*dest_stride);
      for (int x=0; x<src_stride; x+=3)
      {
         *destPixels++ = *srcPixels++;
         *destPixels++ = *srcPixels++;
         *destPixels++ = *srcPixels++;
         *destPixels++ = 255;
      }
   }
}


inline void copyMipRGB(uint32_t width, uint32_t height, uint32_t pad_width, Palette::Data* pal, uint8_t* data, uint8_t* out_data)
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

inline void copyMipRGBA(uint32_t width, uint32_t height, uint32_t pad_width, Palette::Data* pal, uint8_t* data, uint8_t* out_data, uint32_t clamp_a)
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


class LZH {
public:
    static constexpr int BUF_SIZE = 4096;
    static constexpr int LOOK_AHEAD = 60;
    static constexpr int THRESHOLD = 2;
    static constexpr int NUL = BUF_SIZE;
    static constexpr int N_CHAR = (256 - THRESHOLD + LOOK_AHEAD);
    static constexpr int TABLE_SIZE = (N_CHAR * 2 - 1);
    static constexpr int ROOT = (TABLE_SIZE - 1);
    static constexpr int MAX_FREQ = 0x8000;

    LZH() : getbuf(0), getlen(0), putbuf(0), putlen(0), textsize(0), codesize(0),
            printcount(0), match_position(0), match_length(0) {}

   void lzh_unpack(int text_size, MemRStream& in_stream, MemRStream& out_stream);

private:
    uint16_t getbuf, getlen, putbuf, putlen;
    int textsize, codesize, printcount, match_position, match_length;
    std::vector<int> text_buf;
    std::vector<int> freq, prnt, son;

    static const uint8_t D_CODE[256];

    void init_huff_and_tree();
    void start_huff();
    int decode_char(MemRStream& ios);
    int get_bit(MemRStream& ios);
    int get_byte(MemRStream& ios);

    inline int decode_dlen(int i)
    {
        if (i < 32) return 3;
        else if (i < 80) return 4;
        else if (i < 144) return 5;
        else if (i < 192) return 6;
        else if (i < 240) return 7;
        else return 8;
    }

    int decode_position(MemRStream& ios);
    void refill_byte_buf(MemRStream& ios);
    void update(int c);
    void reconst();
};

#endif /* SharedRender_h */
