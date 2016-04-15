//----------------------------------------------------------------------------------
// Copyright (c) 2014 by Board of Trustees of the Leland Stanford, Jr., University
// Author: Alja Mrak-Tadel, Matevz Tadel, Brian Bockelman
//----------------------------------------------------------------------------------
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//----------------------------------------------------------------------------------

#include <math.h>
#include <sstream>
#include <stdio.h>
#include <iostream>
#include <assert.h>

#include "XrdFileCacheIOFileBlock.hh"
#include "XrdFileCache.hh"
#include "XrdFileCacheStats.hh"
#include "XrdFileCacheFactory.hh"

#include "XrdSys/XrdSysError.hh"
#include "XrdSfs/XrdSfsInterface.hh"


using namespace XrdFileCache;

void *PrefetchRunnerBl(void * prefetch_void)
{
   Prefetch *prefetch = static_cast<Prefetch*>(prefetch_void);
   prefetch->Run();
   return NULL;
}

//______________________________________________________________________________
IOFileBlock::IOFileBlock(XrdOucCacheIO &io, XrdOucCacheStats &statsGlobal, Cache & cache)
   : IO(io, statsGlobal, cache)
{
   m_blocksize = Factory::GetInstance().RefConfiguration().m_hdfsbsize;
   GetBlockSizeFromPath();
}

//______________________________________________________________________________
XrdOucCacheIO* IOFileBlock::Detach()
{
   clLog()->Info(XrdCl::AppMsg, "IOFileBlock::Detach() %s", m_io.Path());
   XrdOucCacheIO * io = &m_io;


   for (std::map<int, Prefetch*>::iterator it = m_blocks.begin(); it != m_blocks.end(); ++it)
   {
      m_statsGlobal.Add(it->second->GetStats());
      delete it->second;
   }

   m_cache.Detach(this);  // This will delete us!

   return io;
}

//______________________________________________________________________________
void IOFileBlock::GetBlockSizeFromPath()
{
    const static std::string tag = "hdfsbsize=";
    std::string path= m_io.Path();
    size_t pos1 = path.find(tag);
    size_t t = tag.length();
    if ( pos1 != path.npos)
    {
        pos1 += t;
        size_t pos2 = path.find("&", pos1 );
        if (pos2 != path.npos )
        {
            std::string bs = path.substr(pos1, pos2 - pos1);
            m_blocksize = atoi(bs.c_str());
        }
       else {
            m_blocksize = atoi(path.substr(pos1).c_str());
       }

        clLog()->Debug(XrdCl::AppMsg, "FileBlock::GetBlockSizeFromPath(), blocksize = %lld. %s", m_blocksize, m_io.Path());
    }
}

//______________________________________________________________________________
Prefetch* IOFileBlock::newBlockPrefetcher(long long off, int blocksize, XrdOucCacheIO*  io)
{
   std::string fname;
   m_cache.getFilePathFromURL(io->Path(), fname);
   std::stringstream ss;
   ss << fname;
   char offExt[64];
   // filename like <origpath>___<size>_<offset>
   sprintf(&offExt[0],"___%lld_%lld", m_blocksize, off );
   ss << &offExt[0];
   fname = ss.str();

   clLog()->Debug(XrdCl::AppMsg, "FileBlock::FileBlock(), create XrdFileCachePrefetch. %s", m_io.Path());
   Prefetch* prefetch = new Prefetch(*io, fname, off, blocksize);
   pthread_t tid;
   XrdSysThread::Run(&tid, PrefetchRunnerBl, (void *)prefetch, 0, "BlockFile Prefetcher");

   return prefetch;
}

//______________________________________________________________________________
bool IOFileBlock::ioActive()
{
   bool res = false;
   for (std::map<int, Prefetch*>::iterator it = m_blocks.begin(); it != m_blocks.end(); ++it) {
      if (it->second->InitiateClose())
         res = true;
   }
  
   return res;
}

//______________________________________________________________________________
int IOFileBlock::Read (char *buff, long long off, int size)
{
   long long off0 = off;
   int idx_first = off0/m_blocksize;
   int idx_last = (off0+size-1)/m_blocksize;
   int bytes_read = 0;
   clLog()->Debug(XrdCl::AppMsg, "IOFileBlock::Read() %lld@%d block range [%d-%d] \n %s", off, size, idx_first, idx_last, m_io.Path());

   // protect from reads over the file size
   if (off + size > m_io.FSize())
       size =  m_io.FSize() - off;

   for (int blockIdx = idx_first; blockIdx <= idx_last; ++blockIdx )
   {
      // locate block
      Prefetch* fb;
      m_mutex.Lock();
      std::map<int, Prefetch*>::iterator it = m_blocks.find(blockIdx);
      if ( it != m_blocks.end() )
      {
         fb = it->second;
      }
      else
      {
         size_t pbs = m_blocksize;
         // check if this is last block
         int lastIOFileBlock = (m_io.FSize()-1)/m_blocksize;
         if (blockIdx == lastIOFileBlock )
         {
            pbs =  m_io.FSize() - blockIdx*m_blocksize;
            clLog()->Debug(XrdCl::AppMsg, "IOFileBlock::Read() last block, change output file size to %lld \n %s", pbs, m_io.Path());
         }

         fb = newBlockPrefetcher(blockIdx*m_blocksize, pbs, &m_io);
         m_blocks.insert(std::pair<int,Prefetch*>(blockIdx, (Prefetch*) fb));
      }
      m_mutex.UnLock();

      // edit size if read request is reaching more than a block
      int readBlockSize = size;
      if (idx_first != idx_last)
      {
         if (blockIdx == idx_first)
         {
            readBlockSize = (blockIdx + 1) *m_blocksize - off0;
            clLog()->Debug(XrdCl::AppMsg, "Read partially till the end of the block %s", m_io.Path());
         }
         else if (blockIdx == idx_last)
         {
            readBlockSize = (off0+size) - blockIdx*m_blocksize;
            clLog()->Debug(XrdCl::AppMsg, "Read partially from beginning of block %s", m_io.Path());
         }
         else
         {
            readBlockSize = m_blocksize;
         }
      }
      assert(readBlockSize);

      clLog()->Info(XrdCl::AppMsg, "IOFileBlock::Read() block[%d] read-block-size[%d], offset[%lld] %s", blockIdx, readBlockSize, off, m_io.Path());

      long long min  = blockIdx*m_blocksize;
      if ( off < min) { assert(0); }
      assert(off+readBlockSize <= (min + m_blocksize));
      int retvalBlock = fb->Read(buff, off, readBlockSize);

      clLog()->Debug(XrdCl::AppMsg, "IOFileBlock::Read()  Block read returned %d %s", retvalBlock , m_io.Path());
      if (retvalBlock ==  readBlockSize )
      {
         bytes_read += retvalBlock;
         buff += retvalBlock;
         off += retvalBlock;
      }
      else if (retvalBlock > 0) {
         clLog()->Warning(XrdCl::AppMsg, "IOFileBlock::Read() incomplete read, missing bytes %d %s", readBlockSize-retvalBlock, m_io.Path());
         return bytes_read + retvalBlock;
      }
      else
      {
         clLog()->Error(XrdCl::AppMsg, "IOFileBlock::Read() read error, retval %d %s", retvalBlock, m_io.Path());
         return retvalBlock;
      }
   }

   return bytes_read;
}


