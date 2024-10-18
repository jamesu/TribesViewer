//-----------------------------------------------------------------------------
// Copyright (c) 2018-2024 James S Urquhart.
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
#include <SDL3/SDL.h>
#include <stdio.h>
#include <strings.h>
#include <algorithm>
#include <string>
#include <vector>
#include <cmath>
#include <unordered_map>
#include <slm/slmath.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"

#ifndef PATH_MAX
#define PATH_MAX        4096
#endif

#ifndef NO_BOOST
#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;
#else
#include <filesystem>
namespace fs = std::filesystem;
#endif

#include "CommonShaderTypes.h"
#include "RendererHelper.h"

// The max number of command buffers in flight
static const uint32_t TVMaxBuffersInFlight = 3;

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


#include "CommonData.h"

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
      
      for (Entry& e : mFiles)
      {
         printf("%s\n", e.getFilename(mStringData));
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
   
   template<class T> T* openTypedObject(const char *filename, int32_t forceMount=-1)
   {
      T* obj = NULL;
      MemRStream mem(0, NULL);
      if (openFile(filename, mem, forceMount))
      {
         DarkstarPersistObject *dObj = DarkstarPersistObject::createFromStream(mem);
         if (dObj)
         {
            obj = dynamic_cast<T*>(dObj);
            if (!obj)
               delete dObj;
         }
      }
      return obj;
      
   }
   
   void enumerateVolume(uint32_t idx, std::vector<EnumEntry> &outList, std::vector<std::string> *restrictExts)
   {
      for (Volume::Entry &e : mVolumes[idx]->mFiles)
      {
         if (restrictExts)
         {
            fs::path filePath = e.getFilename(mVolumes[idx]->mStringData);
            std::string  ext = filePath.extension();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            
            bool found = false;
            for (std::string &restrictExt : *restrictExts)
            {
               if (ext == restrictExt)
               {
                  found = true;
                  break;
               }
            }
            
            if (!found)
               continue;
         }
         outList.emplace_back(EnumEntry(e.getFilename(mVolumes[idx]->mStringData), mPaths.size()+idx));
      }
   }
   
   void enumeratePath(uint32_t idx, std::vector<EnumEntry> &outList, std::vector<std::string> *restrictExts)
   {
      for (const fs::directory_entry &itr : fs::directory_iterator(mPaths[idx]))
      {
         if (restrictExts)
         {
            fs::path filePath = itr.path().filename().c_str();
            std::string  ext = filePath.extension();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            
            bool found = false;
            for (std::string &restrictExt : *restrictExts)
            {
               if (ext == restrictExt)
               {
                  found = true;
                  break;
               }
            }
            
            if (!found)
               continue;
         }
         outList.emplace_back(EnumEntry(itr.path().filename().c_str(), idx));
      }
   }
   
   void enumerateFiles(std::vector<EnumEntry> &outList, int restrictIdx=-1, std::vector<std::string> *restrictExt=NULL)
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

class InteriorLight;
class InteriorGeom;

class InteriorGeom : public DarkstarPersistObject
{
public:
   enum
   {
      LowDetail = 0x1
   };
   
   enum PVSFlags
   {
      OutsideZMax = 0x4,
      OutsideYMax = 0x8,
      OutsideXMax = 0x10,
      OutsideZMin = 0x20,
      OutsideYMin = 0x40,
      OutsideXMin = 0x80,
      
      OutsideMin = OutsideZMin | OutsideYMin | OutsideXMin,
      OutsideMax = OutsideZMax | OutsideYMax | OutsideXMax,
      OutsideMask = OutsideMin | OutsideMax
   };
   
   struct Vertex
   {
      uint16_t pIdx;
      uint16_t tIdx;
   };
   
   struct PlaneF
   {
      float x,y,z,d;
   };
   
   struct BSPNode
   {
      uint16_t planeIdx;
      int16_t front;
      int16_t back;
      int16_t fill;
   };
   
   struct BSPLeafSolid
   {
      uint32_t surfIdx;
      uint32_t planeIdx;
      uint16_t numSurfaces;
      uint16_t numPlanes;
   };
   
   struct BSPLeafEmpty
   {
      enum
      {
         External = 0x1,
         PVSMask = 0xFFFE,
         PVSShift = 0x1
      };
      
      uint16_t flags; // incl pvs bits
      uint16_t numSurfs;
      uint32_t pvsIdx;
      uint32_t surfIdx;
      uint32_t planeIdx;
      slm::vec3 mMinBounds;
      slm::vec3 mMaxBounds;
      uint16_t numPlanes;
   };
   
   struct Surface
   {
      enum
      {
         Material = 0x0,
         Link = 0x1,
         TextureBits = 0x1E,
         TextureShift = 0x1,
         AmbientLit = 0x20,
         OutsideVis = 0x40,
         IsFront = 0x80
      };
      
      uint8_t flags;       // 2
      uint8_t materials;   // 3
      uint8_t tsX,tsY;     // 4
      uint8_t toX,toY;     // 6
      uint16_t planeIdx;   // 8
      uint32_t vtxIdx;     // 10
      uint32_t pointIdx;   // 14
      uint8_t numVerts;    // 15
      uint8_t numPoints;   // 16
   };
   
   float mTextureScale;
   slm::vec3 mMinBounds;
   slm::vec3 mMaxBounds;
   int32_t mHighestMip;
   uint32_t mFlags;
   
   std::vector<Surface> mSurfaces;
   std::vector<BSPNode> mBSPNodes;
   std::vector<BSPLeafSolid> mSolidLeafs;
   std::vector<BSPLeafEmpty> mEmptyLeafs;
   std::vector<uint8_t> mPVSBits;
   std::vector<Vertex> mVerts;
   std::vector<slm::vec3> mPoint3List;
   std::vector<slm::vec2> mPoint2List;
   std::vector<PlaneF> mPlanes;
   
   InteriorGeom()
   {
   }
   
   virtual ~InteriorGeom()
   {
   }
   
   bool read(MemRStream &stream, int version)
   {
      uint32_t buildId=0;
      float texScale=0;
      
      assert(version == 7);
      
      stream.read(buildId);
      stream.read(mTextureScale);
      stream.read(mMinBounds);
      stream.read(mMaxBounds);
      
      uint32_t num=0;
      stream.read(num);
      mSurfaces.resize(num);
      stream.read(num);
      mBSPNodes.resize(num);
      stream.read(num);
      mSolidLeafs.resize(num);
      stream.read(num);
      mEmptyLeafs.resize(num);
      stream.read(num);
      mPVSBits.resize(num);
      stream.read(num);
      mVerts.resize(num);
      stream.read(num);
      mPoint3List.resize(num);
      stream.read(num);
      mPoint2List.resize(num);
      stream.read(num);
      mPlanes.resize(num);
      
      stream.read(mSurfaces.size() * sizeof(Surface), &mSurfaces[0]);
      stream.read(mBSPNodes.size() * sizeof(BSPNode), &mBSPNodes[0]);
      stream.read(mSolidLeafs.size() * sizeof(BSPLeafSolid), &mSolidLeafs[0]);
      stream.read(mEmptyLeafs.size() * sizeof(BSPLeafEmpty), &mEmptyLeafs[0]);
      stream.read(mPVSBits.size() * sizeof(uint8_t), &mPVSBits[0]);
      stream.read(mVerts.size() * sizeof(Vertex), &mVerts[0]);
      stream.read(mPoint3List.size() * sizeof(slm::vec3), &mPoint3List[0]);
      stream.read(mPoint2List.size() * sizeof(slm::vec2), &mPoint2List[0]);
      stream.read(mPlanes.size() * sizeof(PlaneF), &mPlanes[0]);
      
      stream.read(mHighestMip);
      stream.read(mFlags);
      
      // NOTE: original code pushes points inside bounding box by 0.0125f if they are on bounding box. Shouldn't be needed here but might be
      // relevant for collision.
      
      return true;
   }
};

class Interior
{
public:
   enum
   {
      IDENT_ITR = 1934775369
   };
   
   struct State
   {
      uint32_t stateNameIdx;
      uint32_t lodIdx;
      uint32_t numLods;
   };
   
   struct Lod
   {
      uint32_t minPixels;
      uint32_t geomNameIdx;
      uint32_t lightStateIdx;
      uint32_t linkableFaces;
   };
   
   std::vector<State> mStates;
   std::vector<Lod> mLods;
   std::vector<uint32_t> mLightStates;
   std::vector<uint32_t> mLodLightStates;
   char* mNames;
   uint32_t mMaterialListNameIdx;
   bool mLinkedInterior;
   slm::vec3 mCenter;
   float mRadius;
   
   std::vector<InteriorLight*> mLightStateInstances;
   std::vector<InteriorLight*> mLodLightStateInstances;
   
   std::vector<InteriorGeom*> mLodGeomInstances;
   
   MaterialList* mMaterials;
   
   const char* getFilename(uint32_t nameIndex)
   {
      return mNames+nameIndex;
   }
   
   Interior()
   {
      mNames = NULL;
   }
   
   ~Interior()
   {
      if (mNames)
         delete[] mNames;
      if (mMaterials)
         delete mMaterials;
   }
   
   bool read(MemRStream &mem)
   {
      IFFBlock block;
      uint32_t num;
      
      mem.read(block);
      if (block.ident != IDENT_ITR)
      {
         return false;
      }
      
      num = 0;
      mem.read(num);
      
      assert(num == 3); // version
      
      num = 0;
      mem.read(num);
      mStates.resize(num);
      if (!mem.read(sizeof(State) * num, &mStates[0]))
         return false;
      
      num = 0;
      mem.read(num);
      mLods.resize(num);
      if (!mem.read(sizeof(Lod) * num, &mLods[0]))
         return false;
      
      num = 0;
      mem.read(num);
      mLodLightStates.resize(num);
      if (!mem.read(sizeof(uint32_t) * num, &mLodLightStates[0]))
         return false;
      
      num = 0;
      mem.read(num);
      mLightStates.resize(num);
      if (!mem.read(sizeof(uint32_t) * num, &mLightStates[0]))
         return false;
      
      num = 0;
      mem.read(num);
      mNames = new char[num];
      if (!mem.read(num, mNames))
         return false;
      
      mem.read(mMaterialListNameIdx);
      mem.read(mLinkedInterior);
      
      return true;
   }
   
   bool loadResources(ResManager* res)
   {
      mMaterials = res->openTypedObject<MaterialList>(getFilename(mMaterialListNameIdx));
      if (!mMaterials)
         return false;
      
      mLodGeomInstances.resize(mLods.size());
      
      for (int i=0; i<mLods.size(); i++)
      {
         mLodGeomInstances[i] = res->openTypedObject<InteriorGeom>(getFilename(mLods[i].geomNameIdx));
      }
      
      if (mLodGeomInstances.size() > 0)
      {
         mCenter = mLodGeomInstances[0]->mMinBounds + ((mLodGeomInstances[0]->mMaxBounds - mLodGeomInstances[0]->mMinBounds) * 0.5);
         mRadius = abs(mCenter.x - mLodGeomInstances[0]->mMaxBounds.x);
      }
      
      
      return true;
   }
   
};

class TerrainBlock;

class TerrainBlockList
{
public:
   enum
   {
      IDENT_GFIL = 1279870535
   };
   
   std::string mBaseName;
   std::string mMLName;
   uint32_t mLastBlockID;
   uint32_t mDetailCount; // max detail levels we can use; also related shift value for int coords
   uint32_t mScale;       // shift value for int coords
   
   // NOTE: bounds appears to be unused
   slm::vec3 mMinBounds;
   slm::vec3 mMaxBounds;

   uint32_t mOrigin[2]; // seems to be an offset for the blocks, but not used for collision?
   uint32_t mSize[2];   // grid block dimensions
   
   struct BlockInfo
   {
      uint32_t ident;
      std::string name;
      TerrainBlock* instance;
      
      BlockInfo()
      {
         ident = 0;
         name = "";
         instance = NULL;
      }
   };
   
   slm::vec2 mGridRange;
   std::vector<int> mBlockMap;
   std::vector<BlockInfo> mBlocks;

   enum BlockMap : uint32_t {
      BM_OneToAll,
      BM_Unique,
      BM_Mosaic
   };

   BlockMap mBlockMapType;

   TerrainBlockList()
   {
      mLastBlockID = 0;
      mDetailCount = 0;
      mScale = 0;
      mMinBounds = slm::vec3(0);
      mMaxBounds = slm::vec3(0);
      mOrigin[0] = 0;
      mOrigin[1] = 0;
      mSize[0] = 0;
      mSize[1] = 0;
      mGridRange = slm::vec2(0);
      mBlockMapType = BM_OneToAll;
   }
   
   virtual ~TerrainBlockList();
   
   inline uint32_t getNumBlocks() const
   {
      return mSize[0] * mSize[1];
   }
   
   inline uint32_t getBlockIndex(int32_t x, int32_t y) const
   {
      return (y * mSize[0]) + x;
   }
   
   inline int32_t getBaseShift() const
   {
      return ((int32_t)mDetailCount - 1);
   }
   
   inline int32_t getBlockShift() const
   {
      // NOTE: shift right to go from world -> local,
      //       shift left to go from local -> world.
      return ((int32_t)mDetailCount - 1) + // base
             mScale; // square size
   }
   
   void loadBlocks(ResManager& mgr, const char* baseName, int volIdx = -1);
   void setSingleBlock(TerrainBlock* block);
   
   bool read(MemRStream &mem)
   {
      uint32_t lastID;
      uint32_t numDetails;
      uint32_t scale;
      uint32_t version;
      
      IFFBlock block;
      mem.read(block);
      if (block.ident != IDENT_GFIL)
      {
         return false;
      }
      
      mem.read(version);
      if (version > 1)
      {
         return false;
      }
      
      mem.readSString32(mMLName);
      mem.read(mLastBlockID);
      mem.read(mDetailCount);
      mem.read(mScale);

      mem.read(mMinBounds);
      mem.read(mMaxBounds);
      mem.read(mOrigin[0]);
      mem.read(mOrigin[1]);
      mem.read(mGridRange);
      mem.read(mSize[0]);
      mem.read(mSize[1]);

      if (version > 0)
      {
         mem.read(mBlockMapType);
      }
      else
      {
         mBlockMapType = BM_OneToAll;
      }

      uint32_t numBlocks = getNumBlocks();
      mBlockMap.resize(numBlocks);
      mem.read(numBlocks * sizeof(uint32_t), &mBlockMap[0]);
      
      numBlocks = 0;
      mem.read(numBlocks);
      mBlocks.resize(numBlocks);
      for (uint32_t i=0; i<numBlocks; i++)
      {
         mem.read(mBlocks[i].ident);
         mem.readSString(mBlocks[i].name);
      }
      
      return true;
   }
};

/*
 NOTE: Internally each grid block in tribes is just a heightmap sized as [y+1][x+1], but with
 [x][y] squares. Most commonly, a 257x257 heightmap with 256x256 squares.
 This is slightly different to torque which uses a fixed 256x256 heightmap which
 repeats (i.e. the height for the right of square 255 is the same as height 0).
 
 For a square at (x,y) the corners use the following heightmap values:
 
 (x+0,y+0)-----(x+0,y+0)
 |                     |
 |                     |
 |                     |
 |                     |
 |                     |
 (x+0,y+1)-----(x+1,y+1)
 
 When handling detail levels, each detail level skips a power of two heightmap values.
 Squares are split at different diagonals using a checkerboard pattern.
 
 Also of note, each square consists of 4 points in the highest detail and 9 in the subsequent detail levels.
 This helps smooth things out in the lower detail levels.
 
 NOTE: Curiously, when SimT2 is present in a mission, Tribes terrain is rendered similar to torques
 terrain block (i.e. with the grid map and the advanced detail levels); otherwise the
 simpler system is used.
 In both cases the underlying geometry of the terrain used for collision is the same.
 
 Bitmaps from the associated material list are used to texture each terrain square; unlike
 torque, no advanced blending occurs at runtime thus there is no extra info like the material alpha map.
 Each square just uses the texture as-is with relevant flips and rotations applied to the texcoords.
 */
class TerrainBlock
{
public:
   enum
   {
      IDENT_GBLK = 1263288903
   };
   
   struct MaterialMap
   {
      uint8_t flag;
      uint8_t matIndex;
      
      enum
      {
         Plain = 0,
         Rotate = 1,
         FlipX = 2,
         FlipY = 4,
         RotateMask = 7,
         //
         EmptyShift = 3,
         EmptyMask = 7
      };
      
      // 0 = not empty, otherwise marks how many details this is empty for.
      int32_t getEmptyDetailLevel()
      {
         return (flag >> EmptyShift) & EmptyMask;
      }
      
      static slm::vec2 sMatCoords[8][4];
      
      /*
       NOTE: texture coords are arranged as follows (assuming opengl convention):
       
       0  7  6
       1  8  5
       2  3  4
       
       Modifier ops occur in the order FlipX, FlipY, Rotate.
       FlipX & FlipY are straight forward.
       Rotate does a clockwise rotation of outer points i.e:
       
       2  1  0
       3  8  7
       4  5  6
       
       Level 0 squares use the outer points, while subsequent detail levels use all the points.
       In addition the relevant square offset is applied for subsequent details.
       */
      static void getBaseTexCoords(uint32_t flag, slm::vec2* outCoords)
      {
         uint8_t idx = flag & RotateMask;
         outCoords[0] = sMatCoords[idx][0];
         outCoords[1] = sMatCoords[idx][1];
         outCoords[2] = sMatCoords[idx][2];
         outCoords[3] = sMatCoords[idx][3];
      }
      
   };
   
   struct GridSquare
   {
      uint16_t flags;
      
      enum
      {
         Split45 = 1,
         Empty = 2,
         HasEmpty = 4
      };
   };
   
   TerrainBlockList* mOwner;
   
   std::string mIdent;
   int32_t mDetailCount; // number of detail levels to use
   int32_t mLightScale;  // shift for lightmap
   slm::vec2 mRange;     // height range
   int32_t mSize[2];     // heightmap dimensions
   
   std::vector<float> mHeightMap;
   std::vector<MaterialMap> mMatMap;
   std::vector<GridSquare> mGridMapBase;
   std::vector<uint8_t> mPinMap[11];
   std::vector<uint16_t> mLightMap;
   
   
   TerrainBlock(TerrainBlockList* owner = NULL) : mOwner(owner)
   {
   }
   
   virtual ~TerrainBlock()
   {
   }
   
   inline uint32_t getLightMapWidth() const
   {
      return (mSize[0] << mLightScale) + 1;
   }
   
   inline uint32_t getHeightMapSize() const
   {
      return (mSize[0] + 1) * (mSize[1] + 1);
   }
   
   inline uint32_t getMatMapSize() const
   {
      return (mSize[0]) * (mSize[1]);
   }
   
   float getHeight(uint32_t x, uint32_t y)
   {
      return mHeightMap[(y*mSize[0]) + x];
   }
   
   void readCompressed(MemRStream& mem, uint32_t size, void* out)
   {
      MemRStream outMem(size, out);
      LZH lzh;
      lzh.lzh_unpack(size, mem, outMem);
   }
   
   bool read(MemRStream &mem)
   {
      IFFBlock block;
      mem.read(block);
      if (block.ident != IDENT_GBLK)
      {
         return false;
      }
      
      uint32_t version = 0;
      mem.read(version);
      if (version > 5)
      {
         return false;
      }

      char ident[17];
      memset(ident, 0, sizeof(ident));
      mem.read(16, &ident[0]);
      mIdent = ident;
      
      mem.read(mDetailCount);
      mem.read(mLightScale);
      mem.read(mRange.x);
      mem.read(mRange.y);
      mem.read(mSize[0]);
      mem.read(mSize[1]);
      
      // Height map
      uint32_t hmSize = getHeightMapSize();
      mHeightMap.resize(hmSize);
      
      if (version == 0)
      {
         mem.read(hmSize * 4, &mHeightMap[0]);
      }
      else if (version < 4)
      {
         // Row compression
         mHeightMap.resize(getHeightMapSize());
         const uint32_t rowSize = mSize[0] + 1;
         
         // leading row
         float* outRow  = &mHeightMap[0];
         mem.read(4 * rowSize, outRow);
         outRow += rowSize;
         
         std::vector<int8_t> offsets;
         offsets.resize(mSize[0]-1);
         
         for (uint32_t i=1; i<mSize[1]; i++)
         {
            float scale = 1.0f;
            float lheight = 1.0f;
            mem.read(scale);
            mem.read(lheight);
            mem.read(mSize[0]-1, &offsets[0]);
            
            // leading height
            *outRow++ = lheight;
            
            // offsets
            for (int8_t offset : offsets)
            {
               lheight += offset * scale;
               *outRow++ = lheight;
            }
            
            // trailing height
            mem.read(lheight);
            *outRow++ = lheight;
         }
         
         // trailing row
         mem.read(4 * rowSize, outRow);
      }
      else
      {
         readCompressed(mem, 4 * getHeightMapSize(), &mHeightMap[0]);
      }

      // Material map
      mMatMap.resize(getMatMapSize());
      
      if (version > 4)
      {
         readCompressed(mem, 2 * getMatMapSize(), &mMatMap[0]);
      }
      else
      {
         mem.read(2 * getMatMapSize(), &mMatMap[0]);
      }
      
      // Pin map
      if (version >= 2)
      {
         for (uint32_t i=0; i<11; i++)
         {
            uint16_t sz = 0;
            mem.read(sz);
            mPinMap[i].resize(sz);
            mem.read(sz, &mPinMap[0]);
         }
      }
      else
      {
         for (uint32_t i=0; i<11; i++)
         {
            mPinMap[i].resize(0);
         }
      }

      if (mLightScale >= 0)
      {
         uint32_t lmWidth = getLightMapWidth();
         mLightMap.resize(lmWidth*lmWidth);
         
         if (version > 4)
         {
            readCompressed(mem, lmWidth*lmWidth*2, &mLightMap[0]);
         }
         else
         {
            mem.read(lmWidth*lmWidth*2, &mLightMap[0]);
         }
      }
      else
      {
         mLightMap.resize(0);
      }

      if (version > 4)
      {
         uint32_t hrlmSize;
         mem.read(hrlmSize);
         
         if (hrlmSize > 0)
         {
            // Can't read HiRes lightmap data
            return false;
            // mem.read(hrlmSize, &mHiResLightmap[0]);
         }
      }
      
      uint32_t hrlmVersion = 0;
      uint32_t numHrlm = 0;
      
      if (version >= 3)
      {
         mem.read(hrlmVersion);
         
         if (hrlmVersion > 0)
         {
            mem.read(numHrlm);
         }
         else
         {
            numHrlm = 0;
         }
      }

      if (version == 3)
      {
         uint32_t cPoolSz = 0;
         uint32_t idxSz = 0;
         uint32_t treeSz = 0;
         
         mem.read(cPoolSz);
         mem.read(idxSz);
         mem.read(treeSz);
         
         if (cPoolSz > 0)
         {
            // Can't read HiRes lightmap data
            return false;
         }
      }
      
      return true;
   }
   
   inline GridSquare* findSquare(int32_t x, int32_t y)
   {
      return (&mGridMapBase[0] +
               (x) +
               (y * mSize[0]));
   }
   
   inline MaterialMap* getMaterialMap(int32_t x, int32_t y)
   {
      return (&mMatMap[0] +
               (x) +
               (y * mSize[0]));
   }
   
   void buildGridMap()
   {
      mGridMapBase.resize(mSize[0] * mSize[1]);
      processGrid();
   }
   
   void processGrid()
   {
      for (int32_t squareX = 0; squareX < mSize[0]; squareX++)
      {
          for (int32_t squareY = 0; squareY < mSize[1]; squareY++)
          {
              GridSquare* sq = findSquare(squareX, squareY);
              processSquare(squareX, squareY, sq);
          }
      }
   }
   
   void processSquare(int32_t squareX, int32_t squareY, GridSquare* sq)
   {
      // NOTE: since we're just rendering the base level here we just factor in whats set in the square
      TerrainBlock::MaterialMap* mat = getMaterialMap(squareX, squareY);
      
      bool emptySet = (mat->flag & TerrainBlock::GridSquare::Empty) != 0;
      bool shouldSplit45 = ((squareX ^ squareY) & 1) == 0;
      
      sq->flags = emptySet ? (GridSquare::Empty | GridSquare::HasEmpty) : 0;
      
      if (shouldSplit45)
      {
          sq->flags |= GridSquare::Split45;
      }
   }
};

// NOTE: pre-gen'd based on flags
slm::vec2 TerrainBlock::MaterialMap::sMatCoords[8][4] = {
   {
      slm::vec2(0.0f, 1.0f),
      slm::vec2(0.0f, 0.0f),
      slm::vec2(1.0f, 0.0f),
      slm::vec2(1.0f, 1.0f),
   },
   {
      slm::vec2(1.0, 1.0),
      slm::vec2(0.0, 1.0),
      slm::vec2(0.0, 0.0),
      slm::vec2(1.0, 0.0)
   },
   {
      slm::vec2(1.0, 1.0),
      slm::vec2(1.0, 0.0),
      slm::vec2(0.0, 0.0),
      slm::vec2(0.0, 1.0)
   },
   {
      slm::vec2(0.0, 1.0), 
      slm::vec2(1.0, 1.0),
      slm::vec2(1.0, 0.0),
      slm::vec2(0.0, 0.0)
   },
   {
      slm::vec2(0.0, 0.0), 
      slm::vec2(0.0, 1.0),
      slm::vec2(1.0, 1.0),
      slm::vec2(1.0, 0.0)
   },
   {
      slm::vec2(1.0, 0.0), 
      slm::vec2(0.0, 0.0),
      slm::vec2(0.0, 1.0),
      slm::vec2(1.0, 1.0)
   },
   {
      slm::vec2(1.0, 0.0),
      slm::vec2(1.0, 1.0),
      slm::vec2(0.0, 1.0),
      slm::vec2(0.0, 0.0)
   },
   {
      slm::vec2(0.0, 0.0), 
      slm::vec2(1.0, 0.0),
      slm::vec2(1.0, 1.0),
      slm::vec2(0.0, 1.0)
   }
};

inline TerrainBlockList::~TerrainBlockList()
{
   for (BlockInfo& info : mBlocks)
   {
      delete info.instance;
   }
}

inline void TerrainBlockList::loadBlocks(ResManager& mgr, const char* baseName, int volIdx)
{
   for (BlockInfo& info : mBlocks)
   {
      MemRStream rStream(0, NULL);
      
      if (info.instance)
      {
         delete info.instance;
         info.instance = NULL;
      }
      
      char buffer[256];
      snprintf(buffer, 256, "%s#%i.dtb", baseName, info.ident);
      
      if (mgr.openFile(buffer, rStream, volIdx))
      {
         info.instance = new TerrainBlock(this);
         if (!info.instance->read(rStream))
         {
            delete info.instance;
            info.instance = NULL;
         }
      }
   }
}

inline void TerrainBlockList::setSingleBlock(TerrainBlock* block)
{
   mBlocks.resize(1);
   mBlockMap.resize(1);
   mBlockMap[0] = 0;
   mBlocks[0].instance = block;
   mScale = 3; // i.e. 8 units per square
   mSize[0] = 1;
   mSize[1] = 1;
}

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
   registerClass("ITRGeometry", &_createClass<InteriorGeom>);
}


class GenericViewer
{
public:
   
   struct LoadedTexture
   {
      int32_t texID;
      uint32_t bmpFlags;
      uint16_t width, height;
      
      LoadedTexture() {;}
      LoadedTexture(int32_t tid, uint32_t bf) : texID(tid), bmpFlags(bf) {;}
   };
   
   struct ActiveMaterial
   {
      LoadedTexture tex;
   };
   
   std::vector<ActiveMaterial> mActiveMaterials;
   std::unordered_map<std::string, LoadedTexture> mLoadedTextures;
   
   ResManager* mResourceManager;
   Palette* mPalette;
   MaterialList* mMaterialList;
   
   bool initVB;
   
   slm::mat4 mProjectionMatrix;
   slm::mat4 mModelMatrix;
   slm::mat4 mViewMatrix;
   
   slm::vec4 mLightColor;
   slm::vec3 mLightPos;
   
   GenericViewer() : mResourceManager(NULL), mPalette(NULL), mMaterialList(NULL)
   {
      
   }
   
   void updateMVP()
   {
      GFXSetModelViewProjection(mModelMatrix, mViewMatrix, mProjectionMatrix);
      GFXSetLightPos(mLightPos, mLightColor);
   }
   
   void initMaterials()
   {
      mActiveMaterials.clear();
      
      if (!mMaterialList)
      {
         assert(false);
         return;
      }
      
      mActiveMaterials.resize(mMaterialList->mMaterials.size());
      for (int i=0; i<mMaterialList->mMaterials.size(); i++)
      {
         Material& mat = mMaterialList->mMaterials[i];
         ActiveMaterial& amat = mActiveMaterials[i];
         loadTexture((const char*)mat.mFilename, amat.tex);
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
            int32_t texID = GFXLoadTexture(bmp, mPalette);
            if (texID >= 0)
            {
               printf("Loaded texture %s dimensions %ix%i\n", filename, bmp->mWidth, bmp->mHeight);
               outTexInfo.bmpFlags = bmp->mFlags;
               outTexInfo.texID = texID;
               outTexInfo.width = bmp->mWidth;
               outTexInfo.height = bmp->mHeight;
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
   
   void clearTextures()
   {
      for (auto itr: mLoadedTextures) { GFXDeleteTexture(itr.second.texID); }
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
            if (mMaterialList) initMaterials();
            return true;
         }
      }
      return false;
   }
   
};

class ShapeViewer : public GenericViewer
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
   
   Shape::Transform& getTransform(uint32_t i)
   {
      return mShape->mTransforms[i];
   }
   
   Shape::Detail& getDetail(uint32_t i)
   {
      return mShape->mDetails[i];
   }
   
   ShapeViewer(ResManager* res)
   {
      mPalette = NULL;
      mShape = NULL;
      mResourceManager = res;
      initVB = false;
   }
   
   ~ShapeViewer()
   {
      for (RuntimeMeshInfo* itr : mRuntimeMeshInfos) { delete itr; }
      for (RuntimeObjectInfo* itr : mRuntimeObjectInfos) { delete itr; }
      if (mPalette) delete mPalette;
      clearVertexBuffer();
      clearTextures();
      clearRender();
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
      mMaterialList = NULL;
   }
   
   void initRender()
   {
      mLightColor = slm::vec4(1,1,1,1);
      mLightPos = slm::vec3(0,2, 2);
      
      // TODO
   }
   
   void clearRender()
   {
      // TODO
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
      
      mMaterialList = inShape.mMaterials;
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
         
         if (bufferVerts.size() > 10000)
         {
            printf("Warning: lots of verts in this model....\n");
            
         }
         
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
      
      GFXLoadModelData(0, &bufferVerts[0], &bufferTVerts[0], &bufferTris[0], bufferVerts.size(), bufferTVerts.size(), bufferTris.size()*3);
   }
   
   void clearVertexBuffer()
   {
      if (!initVB)
         return;
      
      GFXLoadModelData(0, NULL, NULL, NULL, 0, 0, 0);
      initVB = false;
   }
   
   // Rendering
   
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
      updateMVP();
      GFXBeginLinePipelineState();
      GFXDrawLine(start, end, color, width);
   }
   
   void render()
   {
      determineNodeVisibility();
      
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
         uint32_t ofsTexVerts = runtimeMeshInfo->mRealTexVertsPerFrame * runtimeInfo->mTexFrame;
         
         GFXSetModelVerts(0, ofsVerts, ofsTexVerts);
         
         for (CelAnimMesh::Prim& prim: runtimeMeshInfo->mPrims)
         {
            int32_t matIdx = prim.mat;
            if (matIdx < 0)
               continue;
            
            if (matIdx > mActiveMaterials.size())
               matIdx = 0;
            
            if (mActiveMaterials[matIdx].tex.bmpFlags &Bitmap::FLAG_TRANSPARENT)
            {
               GFXBeginModelPipelineState(ModelPipeline_TranslucentBlend, mActiveMaterials[matIdx].tex.texID, 0.65f);
            }
            else if (mActiveMaterials[matIdx].tex.bmpFlags & (Bitmap::FLAG_TRANSLUCENT | Bitmap::FLAG_ADDITIVE | Bitmap::FLAG_SUBTRACTIVE))
            {
               if (mActiveMaterials[matIdx].tex.bmpFlags & Bitmap::FLAG_ADDITIVE)
               {
                  GFXBeginModelPipelineState(ModelPipeline_AdditiveBlend, mActiveMaterials[matIdx].tex.texID, 1.1f);
               }
               else if (mActiveMaterials[matIdx].tex.bmpFlags & Bitmap::FLAG_SUBTRACTIVE)
               {
                  GFXBeginModelPipelineState(ModelPipeline_SubtractiveBlend, mActiveMaterials[matIdx].tex.texID, 1.1f);
               }
               else
               {
                  GFXBeginModelPipelineState(ModelPipeline_TranslucentBlend, mActiveMaterials[matIdx].tex.texID, 1.1f);
               }
            }
            else
            {
               GFXBeginModelPipelineState(ModelPipeline_DefaultDiffuse, mActiveMaterials[matIdx].tex.texID, 1.1f);
            }
            
            GFXDrawModelPrims(prim.numVerts, prim.numInds, prim.startInds, prim.startVerts);
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

class InteriorViewer : public GenericViewer
{
public:
   
   bool initVB;
   uint32_t mLodToRender;
   
   struct RenderInteriorInfo
   {
      InteriorGeom* geom;
      uint32_t startSurf;
      uint32_t numSurfs;
      
      uint32_t startInd;
      uint32_t numTris;
   };
   
   std::vector<RenderInteriorInfo> mRenderInfos;
   
   struct Triangle
   {
      uint16_t i[3];
      Triangle() {;}
   };
   
   struct RuntimeSurf
   {
      RuntimeSurf() : startVert(0), numVerts(0), startInds(0), numInds(0) {;}
      
      uint32_t startVert;
      uint32_t numVerts;
      uint32_t startInds;
      uint32_t numInds;
      uint32_t matIdx;
   };
   
   std::vector<RuntimeSurf> mRuntimeSurfs;
   std::vector<Interior::State> mStates;
   
   InteriorViewer(ResManager* res)
   {
      mResourceManager = res;
      mPalette = NULL;
      mLodToRender = 0;
      //mInterior = NULL;
   }
   
   void render()
   {
      // Render all surfs for now
      mLodToRender = mStates[0].lodIdx;
      
      const RenderInteriorInfo& toRender = mRenderInfos[mLodToRender];
      
      slm::mat4 baseModel = mModelMatrix;
      slm::mat4 y_up = slm::rotation_x(slm::radians(-90.0f));
      mModelMatrix = baseModel * y_up;
      updateMVP();
      
      GFXSetModelVerts(0, 0, 0);
      
      for (int i=toRender.startSurf; i<toRender.startSurf + toRender.numSurfs; i++)
      {
         const RuntimeSurf &surf = mRuntimeSurfs[i];
         
         int32_t matIdx = surf.matIdx;
         if (matIdx < 0)
            continue;
         
         if (matIdx > mActiveMaterials.size())
            matIdx = 0;
         
         if (mActiveMaterials[matIdx].tex.bmpFlags &Bitmap::FLAG_TRANSPARENT)
         {
            GFXBeginModelPipelineState(ModelPipeline_TranslucentBlend, mActiveMaterials[matIdx].tex.texID, 0.65f);
         }
         else if (mActiveMaterials[matIdx].tex.bmpFlags & (Bitmap::FLAG_TRANSLUCENT | Bitmap::FLAG_ADDITIVE | Bitmap::FLAG_SUBTRACTIVE))
         {
            if (mActiveMaterials[matIdx].tex.bmpFlags & Bitmap::FLAG_ADDITIVE)
            {
               GFXBeginModelPipelineState(ModelPipeline_AdditiveBlend, mActiveMaterials[matIdx].tex.texID, 1.1f);
            }
            else if (mActiveMaterials[matIdx].tex.bmpFlags & Bitmap::FLAG_SUBTRACTIVE)
            {
               GFXBeginModelPipelineState(ModelPipeline_SubtractiveBlend, mActiveMaterials[matIdx].tex.texID, 1.1f);
            }
            else
            {
               GFXBeginModelPipelineState(ModelPipeline_TranslucentBlend, mActiveMaterials[matIdx].tex.texID, 1.1f);
            }
         }
         else
         {
            GFXBeginModelPipelineState(ModelPipeline_DefaultDiffuse, mActiveMaterials[matIdx].tex.texID, 1.1f);
         }
         
         GFXDrawModelPrims(surf.numVerts, surf.numInds, surf.startInds, surf.startVert);
      }
      
      mModelMatrix = baseModel;
   }
   
   void loadInterior(Interior& inInterior)
   {
      inInterior.loadResources(mResourceManager);
      mRuntimeSurfs.clear();
      mRenderInfos.clear();
      mStates = inInterior.mStates;
      mMaterialList = inInterior.mMaterials;
      
      std::vector<slm::vec3> verts;
      std::vector<slm::vec2> tverts;
      std::vector<Triangle> tris;
      RuntimeSurf surf;
      
      // Need to load materials before since we need size info from them
      initMaterials();
      
      // textureScaleBits = 4
      
      // Prepare surface data
      for (InteriorGeom* geom : inInterior.mLodGeomInstances)
      {
         RenderInteriorInfo info;
         info.geom = geom;
         info.startSurf = mRuntimeSurfs.size();
         info.numSurfs = geom->mSurfaces.size();
         info.startInd = tris.size()*3;
         info.numTris = 0;
         int maxMipLevel = 0;
         
         for (InteriorGeom::Surface &isurf : geom->mSurfaces)
         {
            InteriorGeom::PlaneF plane = geom->mPlanes[isurf.planeIdx];
            
            uint32_t startVert = verts.size()/2;
            
            slm::vec3 surfNormal = slm::vec3(plane.x, plane.y, plane.z);
            surfNormal.normalize();
            
            surf.startVert = 0;
            surf.startInds = tris.size() * 3;
            surf.numInds = 0;
            surf.matIdx = isurf.materials;
            
            uint16_t lastVert = 0;
            ActiveMaterial& amat = mActiveMaterials[surf.matIdx];
            
            // Tribes offsets and scales texture coords based on these surface params.
            // The scale value is what size the texture should be from 0...1 (starting from 1), while the offset is
            // how many texels the texture should be offset by (relative to the size of the loaded texture)
            slm::vec2 txScale(1,1);
            slm::vec2 txOffset(0,0);
            
            txScale = slm::vec2(((float)(((int)isurf.tsX+1) << maxMipLevel)) / (float)amat.tex.width,
                                ((float)(((int)isurf.tsY+1) << maxMipLevel)) / (float)amat.tex.height);
            txOffset = slm::vec2((float)isurf.toX / (float)amat.tex.width,
                                 (float)isurf.toY / (float)amat.tex.height);
            
            // First add all the verts
            for (int i=(int)isurf.vtxIdx; i<((int)isurf.vtxIdx) + ((int)isurf.numVerts); i++)
            {
               InteriorGeom::Vertex& vert = geom->mVerts[i];
               
               slm::vec2 tv = geom->mPoint2List[vert.tIdx];
               tv *= txScale;
               tv += txOffset;
               
               // Add the two new verts
               verts.push_back(geom->mPoint3List[vert.pIdx]);
               verts.push_back(surfNormal);
               tverts.push_back(tv);
               
               surf.numVerts++;
            }
            
            // Now add all the indices
            // Need to insert a triangle fan for each vert starting from the origin
            for (int i=1; i < ((int)isurf.numVerts)-1; i++)
            {
               Triangle t;
               
               t.i[0] = startVert;
               t.i[1] = startVert+i;
               t.i[2] = startVert+i+1;
               tris.push_back(t);
               
               surf.numInds += 3;
               info.numTris += 1;
            }
            
            mRuntimeSurfs.push_back(surf);
         }
         
         for (RuntimeSurf &surf : mRuntimeSurfs)
         {
            surf.numVerts = verts.size();
         }
         
         mRenderInfos.push_back(info);
      }
      
      assert(verts.size() < 0xFFFF);
      GFXLoadModelData(0, &verts[0], &tverts[0], &tris[0], verts.size(), tverts.size(), tris.size()*3);
   }
   
   void clear()
   {
      clearTextures();
      mMaterialList = NULL;
   }
   
};

class ViewController
{
public:
   slm::vec3 mViewPos;
   slm::vec3 mCamRot;
   
   virtual void update(float dt) = 0;
   virtual bool isResourceLoaded() = 0;
};

class InteriorViewerController : public ViewController
{
public:
   InteriorViewer mViewer;
   SDL_Window* mWindow;
   float xRot, yRot, mDetailDist;
   
   Interior* mInterior;
   std::string mPaletteName;
   
   InteriorViewerController(SDL_Window* window, ResManager* mgr) :
   mViewer(mgr), mInterior(NULL), mWindow(window)
   {
      xRot = 0.0f;
      yRot = 0.0f;
      mCamRot = slm::vec3(0,0,0);
      mDetailDist = 0.0f;
      mPaletteName = "ice.day.ppl";
   }
   
   ~InteriorViewerController()
   {
      if (mInterior)
         delete mInterior;
   }
   
   bool isResourceLoaded()
   {
      return mInterior != NULL;
   }
   
   void loadInterior(const char* filename, int volIdx=-1)
   {
      MemRStream rStream(0, NULL);
      mViewer.clear();
      if (mInterior)
         delete mInterior;
      mInterior = NULL;
      
      if (mViewer.mResourceManager->openFile(filename, rStream, volIdx))
      {
         Interior* obj = new Interior();
         if (!obj->read(rStream))
         {
            delete obj;
            return;
         }
         
         mInterior = ((Interior*)obj);
         mViewer.clear();
         mViewer.setPalette(mPaletteName.c_str());
         mViewer.loadInterior(*mInterior);
         
         mViewPos = slm::vec3(0, mInterior->mCenter.z, mInterior->mRadius);
      }
   }
   
   void update(float dt)
   {
      mViewer.mModelMatrix = slm::rotation_x(xRot) * slm::rotation_y(yRot);
      slm::mat4 rotMat = slm::rotation_z(slm::radians(mCamRot.z)) * slm::rotation_y(slm::radians(mCamRot.y)) *  slm::rotation_x(slm::radians(mCamRot.x));
      rotMat = inverse(rotMat);
      mViewer.mViewMatrix = slm::mat4(1) * rotMat * slm::translation(-mViewPos);
      
      int w, h;
      SDL_GetWindowSize(mWindow, &w, &h);
      mViewer.mProjectionMatrix = slm::perspective_fov_rh( slm::radians(90.0), (float)w/(float)h, 0.01f, 10000.0f);
      
      mViewer.render();
   }
};

class TerrainViewer : public GenericViewer
{
public:
   
   TerrainBlockList* mBlockList;
   TerrainBlock* mBlock;
   TerrainBlockList mSingleList;
   
   std::string mLastMLName;
   std::vector<uint32_t> mBlockModels;
   
   TerrainViewer(ResManager* res)
   {
      mResourceManager = res;
      mPalette = NULL;
      mLastMLName = "";
   }
   
   ~TerrainViewer()
   {
      clear();
   }
   
   
   void renderBlock(TerrainBlock& block, float squareSize)
   {
      // Setup pipeline state and matrices
      GFXBeginModelPipelineState(ModelPipeline_DefaultDiffuse, 0, 1.1f);
      GFXSetModelViewProjection(mModelMatrix, mViewMatrix, mProjectionMatrix);
      
      static std::vector<slm::vec3> verts;
      static std::vector<slm::vec2> texverts;
      static std::vector<uint16_t> inds;
      
      uint32_t usedModels = 0;
      
      // Clear buffers
      verts.clear();
      texverts.clear();
      inds.clear();
      
      uint32_t vertCounter = 0;
      uint32_t squareCounter = 0;
      const uint32_t maxSquaresPerDraw = 64;
      
      struct DrawBatch
      {
         uint32_t squareIndexes[64];
         uint16_t squareFlags[64];
         uint16_t matFlags[64];
         uint32_t matIndex;
         uint32_t usedSquares;
         uint32_t modelId;
      };
      
      static std::vector<DrawBatch> drawBatches;
      
      // Create batches
      for (int y = 0; y < block.mSize[1]; y+= 8)
      {
         int yLast = std::min<int>(y + 8, block.mSize[1]);
         int yCount = yLast - y;
         
         for (int x = 0; x < block.mSize[0]; x+= 8)
         {
            int xLast = std::min<int>(x + 8, block.mSize[0]);
            int xCount = xLast - x;
            
            TerrainBlock::GridSquare* sq = block.findSquare(x,y);
            TerrainBlock::MaterialMap* mat = block.getMaterialMap(x,y);
            
            bool newBatch = true;
            
            for (DrawBatch& batch : drawBatches)
            {
               if (batch.matIndex == mat->matIndex && batch.usedSquares < maxSquaresPerDraw)
               {
                  batch.squareIndexes[batch.usedSquares] = (y * block.mSize[0]) + x;
                  batch.squareFlags[batch.usedSquares] = sq->flags;
                  batch.matFlags[batch.usedSquares] = mat->flag;
                  batch.usedSquares++;
                  newBatch = false;
                  break;
               }
            }
            
            if (newBatch)
            {
               DrawBatch newBatch = {};
               newBatch.matIndex = mat->matIndex;
               newBatch.squareIndexes[0] = (y * block.mSize[0]) + x;
               newBatch.squareFlags[0] = sq->flags;
               newBatch.matFlags[0] = mat->flag;
               newBatch.usedSquares = 1;
               newBatch.modelId = drawBatches.size();
               
               if (std::find(mBlockModels.begin(), mBlockModels.end(), newBatch.modelId) == mBlockModels.end())
               {
                  mBlockModels.push_back(newBatch.modelId);
               }
               
               drawBatches.push_back(newBatch);
            }
         }
      }
      
      // Draw batches
      for (DrawBatch& batch : drawBatches)
      {
         for (uint32_t i=0; i<batch.usedSquares; i++)
         {
            uint32_t x = batch.squareIndexes[i] % block.mSize[0];
            uint32_t y = batch.squareIndexes[i] / block.mSize[0];
            
            // Calc square heights
            float h1 = block.getHeight(x, y);
            float h2 = block.getHeight((x + 1), y);
            float h3 = block.getHeight(x, (y + 1));
            float h4 = block.getHeight((x + 1), (y + 1));
            
            // Add vertices for square corners
            slm::vec3 vp(x * squareSize, y * squareSize, 0.0f);
            verts.push_back(vp + slm::vec3(0, 0, h1));
            verts.push_back(vp + slm::vec3(squareSize, 0, h2));
            verts.push_back(vp + slm::vec3(0, squareSize, h3));
            verts.push_back(vp + slm::vec3(squareSize, squareSize, h4));
            
            // Texcoords (material)
            slm::vec2 texCoords[4];
            TerrainBlock::MaterialMap::getBaseTexCoords(batch.matFlags[i], &texCoords[0]);
            for (uint32_t i=0; i<4; i++)
            {
               texverts.push_back(texCoords[i]);
            }
            
            // Determine indices based on Split45 flag
            // TODO: verify these indices are correct
            if ((batch.squareFlags[i] & TerrainBlock::GridSquare::Split45) != 0)
            {
               inds.push_back(vertCounter + 0); 
               inds.push_back(vertCounter + 2);
               inds.push_back(vertCounter + 3);
               //
               inds.push_back(vertCounter + 0);
               inds.push_back(vertCounter + 3);
               inds.push_back(vertCounter + 1);
            }
            else
            {
               inds.push_back(vertCounter + 0); 
               inds.push_back(vertCounter + 1);
               inds.push_back(vertCounter + 2);
               //
               inds.push_back(vertCounter + 1);
               inds.push_back(vertCounter + 3);
               inds.push_back(vertCounter + 2);
            }
         
            uint32_t modelId = batch.modelId;
            
            // Load model data into GPU
            GFXLoadModelData(modelId,
                             verts.data(),
                             texverts.data(),
                             inds.data(),
                             verts.size(),
                             texverts.size(),
                             inds.size());
            
            GFXBeginModelPipelineState(ModelPipeline_DefaultDiffuse, mActiveMaterials[batch.matIndex].tex.texID, 1.1f);
            GFXSetModelVerts(modelId, 0, 0);
            GFXDrawModelPrims(verts.size() / 3, inds.size(), 0, 0);
            
            // Clear the buffers for the next batch
            verts.clear();
            texverts.clear();
            inds.clear();
         }
      }
   }
   
   void render()
   {
      // Render all surfs for now
      slm::mat4 baseModel = mModelMatrix;
      slm::mat4 y_up = slm::rotation_x(slm::radians(-90.0f));
      mModelMatrix = baseModel * y_up;
      updateMVP();
      
      // TODO: use the grid
      if (mBlock)
      {
         renderBlock(*mBlock,
                     1<<mBlockList->mScale);
         
      }
      
      mModelMatrix = baseModel;
   }
   
   void updateMaterials()
   {
      if (mBlockList == NULL)
         return;
      
      if (mMaterialList == NULL || mLastMLName != mBlockList->mMLName)
      {
         mLastMLName = mBlockList->mMLName;
         mMaterialList = mResourceManager->openTypedObject<MaterialList>(mBlockList->mMLName.c_str());
         initMaterials();
      }
   }
   
   void clear()
   {
      for (uint32_t modelId : mBlockModels)
      {
         GFXClearModelData(modelId);
      }
      mBlockModels.clear();
   
      if (mBlockList != &mSingleList)
      {
         delete mBlockList;
         mBlock = NULL;
      }
      else
      {
         delete mBlock;
      }
      
      mBlock = NULL;
      mBlockList = NULL;
      mSingleList.setSingleBlock(NULL);
      clearTextures();
      mMaterialList = NULL;
   }
};



class TerrainViewerController : public ViewController
{
public:
   SDL_Window* mWindow;
   float xRot, yRot, mDetailDist;
   
   std::string mPaletteName;
   TerrainViewer mViewer;
   
   TerrainViewerController(SDL_Window* window, ResManager* mgr) : mWindow(window), mViewer(mgr)
   {
      xRot = 0.0f;
      yRot = 0.0f;
      mCamRot = slm::vec3(0,0,0);
      mDetailDist = 0.0f;
      mPaletteName = "ice.day.ppl";
   }
   
   ~TerrainViewerController()
   {
      mViewer.clear();
   }
   
   void loadGrid(const char* filename, int volIdx=-1)
   {
      MemRStream rStream(0, NULL);
      mViewer.clear();
      
      if (mViewer.mResourceManager->openFile(filename, rStream, volIdx))
      {
         mViewer.mBlockList = new TerrainBlockList();
         if (mViewer.mBlockList->read(rStream))
         {
            std::string baseName = filename;
            std::size_t dot_pos = baseName.find_last_of('.');

            if (dot_pos != std::string::npos)
            {
               baseName = baseName.substr(0, dot_pos);
            }
            
            mViewer.mBlockList->loadBlocks(*mViewer.mResourceManager, baseName.c_str(), volIdx);
         }
         else
         {
            delete mViewer.mBlockList;
            mViewer.mBlockList = NULL;
         }
      }
      
      mViewer.updateMaterials();
   }
   
   void loadSingleBlock(const char* filename, int volIdx=-1)
   {
      MemRStream rStream(0, NULL);
      mViewer.clear();
      
      if (mViewer.mResourceManager->openFile(filename, rStream, volIdx))
      {
         mViewer.mBlock = new TerrainBlock();
         if (mViewer.mBlock->read(rStream))
         {
            mViewer.mBlockList = &mViewer.mSingleList;
            mViewer.mBlockList->setSingleBlock(mViewer.mBlock);
         }
         else
         {
            delete mViewer.mBlock;
            mViewer.mBlock = NULL;
         }
      }
      
      mViewer.updateMaterials();
   }
   
   
   bool isResourceLoaded()
   {
      return !(mViewer.mBlockList == NULL && mViewer.mBlock == NULL);
   }
   
   void update(float dt)
   {
      mViewer.mModelMatrix = slm::rotation_x(xRot) * slm::rotation_y(yRot);
      slm::mat4 rotMat = slm::rotation_z(slm::radians(mCamRot.z)) * slm::rotation_y(slm::radians(mCamRot.y)) *  slm::rotation_x(slm::radians(mCamRot.x));
      rotMat = inverse(rotMat);
      mViewer.mViewMatrix = slm::mat4(1) * rotMat * slm::translation(-mViewPos);
      
      int w, h;
      SDL_GetWindowSize(mWindow, &w, &h);
      mViewer.mProjectionMatrix = slm::perspective_fov_rh( slm::radians(90.0), (float)w/(float)h, 0.01f, 10000.0f);
      
      mViewer.render();
   }
};

class ShapeViewerController : public ViewController
{
public:
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
   
   ShapeViewerController(SDL_Window* window, ResManager* mgr) :
   mViewer(mgr)
   {
      mViewPos = slm::vec3(0,0,0);
      mCamRot = slm::vec3(0,0,0);
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
   
   bool isResourceLoaded()
   {
      return mShape != NULL;
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
            if (!mViewer.setPalette(mPaletteName.c_str()))
            {
               printf("Warning: cant load palette %s\n", mPaletteName.c_str());
            }
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
      mViewer.mModelMatrix = slm::rotation_x(xRot) * slm::rotation_y(yRot);
      slm::mat4 rotMat = slm::rotation_z(slm::radians(mCamRot.z)) * slm::rotation_y(slm::radians(mCamRot.y)) *  slm::rotation_x(slm::radians(mCamRot.x));
      rotMat = inverse(rotMat);
      mViewer.mViewMatrix = slm::mat4(1) * rotMat * slm::translation(-mViewPos);
      
      int w, h;
      SDL_GetWindowSize(mWindow, &w, &h);
      mViewer.mProjectionMatrix = slm::perspective_fov_rh( slm::radians(90.0), (float)w/(float)h, 0.01f, 10000.0f);
      
      if (!mManualThreads)
         mViewer.advanceThreads(dt);
      mViewer.selectDetail(mDetailDist, w, h);
      mViewer.animateNodes();
      mViewer.render();
      if (mRenderNodes)
      {
         mViewer.renderNodes(mShape->mDetails[mViewer.mCurrentDetail].rootNode, slm::vec3(0,0,0), mHighlightNodeIdx);
      }
      
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


static const uint64_t tickMS = 1000.0 / 60;

struct MainState
{
   ResManager resManager;
   ShapeViewerController* shapeController;
   InteriorViewerController* interiorController;
   TerrainViewerController* terrainController;
   ViewController *currentController;
   
   //
   slm::vec3 deltaMovement;
   slm::vec3 deltaRot;
   slm::vec3 testPos;
   uint64_t lastTicks;
   
   int selectedFileIdx;
   int selectedVolumeIdx;
   std::vector<ResManager::EnumEntry> fileList;
   std::vector<std::string> restrictExtList;
   std::vector<const char*> cFileList;
   std::vector<std::string> sFileList;
   std::vector<const char*> cVolumeList;
   
   int oldSelectedVolumeIdx;
   int oldSelectedFileIdx;
   //
   
   int in_argc;
   const char** in_argv;
   
   bool isGFXSetup;
   bool running;
   
   SDL_Window* window;
   
   MainState() : shapeController(NULL), interiorController(NULL), terrainController(NULL), currentController(NULL), in_argc(0), isGFXSetup(false)
   {
      lastTicks = 0;
      selectedFileIdx = -1;
      selectedVolumeIdx = -1;
      oldSelectedVolumeIdx = -1;
      oldSelectedFileIdx = -1;
      running = false;
      window = NULL;
      
      testPos = slm::vec3(0);
   }
   
   void init(SDL_Window* in_window, int argc, const char** argv)
   {
      window = in_window;
      in_argc = argc;
      in_argv = argv;
      
      shapeController = new ShapeViewerController(window, &resManager);
      interiorController = new InteriorViewerController(window, &resManager);
      terrainController = new TerrainViewerController(window, &resManager);
   }
   
   int boot();
   int loop();
   
   int testBoot();
   int testLoop();
   
   void shutdown();
};

static MainState gMainState;


int main(int argc, const char * argv[])
{
   SDL_Window* window = NULL;
   SDL_Renderer* renderer = NULL;
   
   assert(sizeof(slm::vec2) == 8);
   assert(sizeof(slm::vec3) == 12);
   assert(sizeof(slm::vec4) == 16);
   
   DarkstarPersistObject::initStatics();
   
   if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
      printf("Couldn't initialize SDL: %s\n", SDL_GetError());
      return (1);
   }
   
   if (!SDL_CreateWindowAndRenderer("DTS Viewer", 1024, 700, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE, &window, &renderer)) {
      printf( "Window could not be created! SDL_Error: %s\n", SDL_GetError() );
      return (1);
   }
   
   // Init basic main
   gMainState.init(window, argc, argv);
   
   int setupCode = GFXSetup(window, renderer);
   
   if (setupCode < 0)
   {
      return 1;
   }
   
   // Non-Emscripten setup
   while (setupCode != 0)
   {
      setupCode = GFXSetup(window, renderer);
   }
   
   int ret = gMainState.boot();
   if (ret != 0)
      return ret;
   
   while (gMainState.loop() == 0)
   {
      ;
   }
   
   gMainState.shutdown();
   
   return 0;
}

void MainState::shutdown()
{
   if (shapeController)
   {
      delete shapeController;
      delete interiorController;
      delete terrainController;
      shapeController = NULL;
      interiorController = NULL;
      terrainController = NULL;
   }
   
   GFXTeardown();
   SDL_DestroyWindow( gMainState.window );
   SDL_Quit();
}

int MainState::boot()
{
   currentController = shapeController;
   
   for (int i=1; i<in_argc; i++)
   {
      const char *path = in_argv[i];
      if (path && path[0] == '-')
         break;
      
      fs::path filePath = path;
      std::string  ext = filePath.extension();
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      
      if (ext == ".dts")
      {
         shapeController->loadShape(path);
         currentController = shapeController;
      }
      else if (ext == ".vol" || ext == ".ted")
      {
         resManager.addVolume(path);
      }
      else if (ext == ".ppl" || ext == ".pal")
      {
         shapeController->mPaletteName = path;
         interiorController->mPaletteName = path;
         terrainController->mPaletteName = path;
      }
      else if (ext == ".dis")
      {
         interiorController->loadInterior(path);
         currentController = interiorController;
      }
      else if (ext == ".dtf")
      {
         terrainController->loadGrid(path);
         currentController = terrainController;
      }
      else if (ext == ".dtb")
      {
         terrainController->loadSingleBlock(path);
         currentController = terrainController;
      }
      else if (ext == "")
      {
         resManager.mPaths.emplace_back(path);
      }
   }
   
   if (!currentController->isResourceLoaded())
   {
      fprintf(stderr, "please specify a starting shape or interior or terrain to load\n");
      return 1;
   }
   
   running = true;
   
   deltaMovement = slm::vec3(0);
   deltaRot = slm::vec3(0);
   lastTicks = SDL_GetTicks();
   
   selectedFileIdx = -1;
   selectedVolumeIdx = -1;
   restrictExtList.clear();
   fileList.clear();
   restrictExtList.push_back(".dts");
   restrictExtList.push_back(".dis");
   restrictExtList.push_back(".dtb");
   restrictExtList.push_back(".dtf");
   resManager.enumerateFiles(fileList, selectedVolumeIdx, &restrictExtList);
   sFileList.resize(fileList.size());
   
   for (int i=0; i<fileList.size(); i++)
   {
      sFileList[i] = fileList[i].filename;
   }
   
   for (int i=0; i<fileList.size(); i++)
   {
      cFileList.push_back(sFileList[i].c_str());
   }
   
   resManager.enumerateSearchPaths(cVolumeList);
   
   oldSelectedVolumeIdx = -1;
   oldSelectedFileIdx = -1;
   
   return 0;
}

int MainState::loop()
{
   if (!running)
      return 1;
   
   ImGui::StyleColorsDark();
   
   SDL_Event event;
   
   uint64_t curTicks = SDL_GetTicks();
   uint64_t oldLastTicks = lastTicks;
   float dt = ((float)(curTicks - lastTicks)) / 1000.0f;
   lastTicks = curTicks;
   
   currentController->mCamRot += deltaRot * dt * 100;
   slm::mat4 rotMat = slm::rotation_z(slm::radians(currentController->mCamRot.z)) * slm::rotation_y(slm::radians(currentController->mCamRot.y)) *  slm::rotation_x(slm::radians(currentController->mCamRot.x));
   //rotMat = inverse(rotMat);
   slm::vec4 forwardVec = rotMat * slm::vec4(deltaMovement, 1);
   currentController->mViewPos += forwardVec.xyz() * dt;
   
   int w, h;
   SDL_GetWindowSize(window, &w, &h);
   
   //glViewport(0,0,w,h);
   
   if (oldSelectedVolumeIdx != selectedVolumeIdx)
   {
      fileList.clear();
      resManager.enumerateFiles(fileList, selectedVolumeIdx, &restrictExtList);
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
      fs::path filePath = cFileList[selectedFileIdx];
      std::string  ext = filePath.extension();
      
      if (ext == ".dis")
      {
         interiorController->loadInterior(cFileList[selectedFileIdx], selectedVolumeIdx);
         currentController = interiorController;
      }
      else if (ext == ".dtf")
      {
         terrainController->loadGrid(cFileList[selectedFileIdx], selectedVolumeIdx);
         currentController = terrainController;
      }
      else if (ext == ".dtb")
      {
         terrainController->loadSingleBlock(cFileList[selectedFileIdx], selectedVolumeIdx);
         currentController = terrainController;
      }
      else
      {
         shapeController->loadShape(cFileList[selectedFileIdx], selectedVolumeIdx);
         currentController = shapeController;
      }
      oldSelectedFileIdx = selectedFileIdx;
   }
   
   while (SDL_PollEvent(&event))
   {
      ImGui_ImplSDL3_ProcessEvent(&event);
      
      switch (event.type)
      {
         case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
         case SDL_EVENT_WINDOW_RESIZED:
            GFXHandleResize();
            break;
            
         case SDL_EVENT_KEY_DOWN:
         case SDL_EVENT_KEY_UP:
         {
            slm::vec3 forwardVec = slm::vec3();
            switch (event.key.key)
            {
               case SDLK_A:  deltaMovement.x = event.type == SDL_EVENT_KEY_DOWN ? -1 : 0; break;
               case SDLK_D:  deltaMovement.x = event.type == SDL_EVENT_KEY_DOWN ? 1 : 0; break;
               case SDLK_Q:  deltaMovement.y = event.type == SDL_EVENT_KEY_DOWN ? 1 : 0; break;
               case SDLK_E:  deltaMovement.y = event.type == SDL_EVENT_KEY_DOWN ? -1 : 0; break;
               case SDLK_W:  deltaMovement.z = event.type == SDL_EVENT_KEY_DOWN ? -1 : 0; break;
               case SDLK_S:  deltaMovement.z = event.type == SDL_EVENT_KEY_DOWN ? 1 : 0; break;
               case SDLK_LEFT:  deltaRot.y = event.type == SDL_EVENT_KEY_DOWN ? 1 : 0; break;
               case SDLK_RIGHT: deltaRot.y = event.type == SDL_EVENT_KEY_DOWN ? -1 : 0; break;
               case SDLK_UP:  deltaRot.x = event.type == SDL_EVENT_KEY_DOWN ? 1 : 0; break;
               case SDLK_DOWN: deltaRot.x = event.type == SDL_EVENT_KEY_DOWN ? -1 : 0; break;
            }
         }
            break;
            
         case SDL_EVENT_QUIT:
            running = false;
            break;
      }
   }
   
   if (GFXBeginFrame())
   {
      currentController->update(dt);
      
      ImGui::Begin("Browse");
      ImGui::Columns(2);
      ImGui::ListBox("##bvols", &selectedVolumeIdx, &cVolumeList[0], cVolumeList.size());
      ImGui::NextColumn();
      ImGui::ListBox("##bfiles", &selectedFileIdx, &cFileList[0], cFileList.size());
      ImGui::End();
      
      GFXEndFrame();
   }
   else
   {
      lastTicks = oldLastTicks;
   }
   
   uint64_t endTicks = SDL_GetTicks();
   if (endTicks - lastTicks < tickMS)
   {
      SDL_Delay(tickMS - (endTicks - lastTicks));
   }
   
   return running ? 0 : 1;
}

int MainState::testBoot()
{
   lastTicks = SDL_GetTicks();
   testPos = slm::vec3(0,0,0);
   deltaMovement = slm::vec3(0,0,0);
   deltaRot = slm::vec3(0,0,0);
}

int MainState::testLoop()
{
   if (!running)
      return 1;
   
   uint64_t curTicks = SDL_GetTicks();
   float dt = ((float)(curTicks - lastTicks)) / 1000.0f;
   lastTicks = curTicks;
   
   int w, h;
   SDL_GetWindowSize(window, &w, &h);
   
   testPos += deltaMovement * dt * 100;
   
   //SDL_SetRenderDrawColor(renderer, 255, 255, 255, 0);
   //SDL_RenderClear(renderer);
   //SDL_RenderCopy(renderer, bitmapTex, NULL, NULL);
   
   GFXTestRender(testPos);
   
   //SDL_RenderPresent(renderer);
   
   SDL_Event event;
   while (SDL_PollEvent(&event))
   {
      switch (event.type)
      {
         case SDL_EVENT_KEY_DOWN:
         case SDL_EVENT_KEY_UP:
         {
            switch (event.key.key)
            {
               case SDLK_A:  deltaMovement.x = event.type == SDL_EVENT_KEY_DOWN ? -1 : 0; break;
               case SDLK_S: deltaMovement.x = event.type == SDL_EVENT_KEY_DOWN ? 1 : 0; break;
               case SDLK_Q:    deltaMovement.y = event.type == SDL_EVENT_KEY_DOWN ? 1 : 0; break;
               case SDLK_E:  deltaMovement.y = event.type == SDL_EVENT_KEY_DOWN ? -1 : 0; break;
               case SDLK_W:  deltaMovement.z = event.type == SDL_EVENT_KEY_DOWN ? -1 : 0; break;
               case SDLK_D:  deltaMovement.z = event.type == SDL_EVENT_KEY_DOWN ? 1 : 0; break;
            }
         }
            break;
            
         case SDL_EVENT_QUIT:
            running = false;
            break;
      }
   }
   
   return running ? 0 : 1;
}
