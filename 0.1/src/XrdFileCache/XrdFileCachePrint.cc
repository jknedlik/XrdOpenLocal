//----------------------------------------------------------------------------------
// Copyright (c) 2014 by Board of Trustees of the Leland Stanford, Jr., University
// Author: Alja Mrak-Tadel
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

#include <iostream>
#include <fcntl.h>
#include <vector>
#include "XrdFileCachePrint.hh"
#include "XrdOuc/XrdOucEnv.hh"
#include "XrdOuc/XrdOucStream.hh"
#include "XrdOuc/XrdOucArgs.hh"
#include "XrdOfs/XrdOfsConfigPI.hh"
#include "XrdSys/XrdSysLogger.hh"
#include "XrdFileCacheInfo.hh"
#include "XrdOss/XrdOss.hh"

using namespace XrdFileCache;

Print::Print(XrdOss* oss, bool v, const char* path): m_oss(oss), m_verbose(v), m_ossUser("nobody")
{
   if (isInfoFile(path)) {
      printFile(std::string(path));
   }
   else {
      XrdOssDF* dh = m_oss->newDir(m_ossUser);
      if ( dh->Opendir(path, m_env)  >= 0 ) {
         printDir(dh, path);
      }
   }

}


bool Print::isInfoFile(const char* path) {
   if (strncmp(&path[strlen(path)-6], ".cinfo", 6))
      return false;
   return true;
}


void Print::printFile(const std::string& path) 
{ 
   printf("printing %s ...\n", path.c_str());
   XrdOssDF* fh = m_oss->newFile(m_ossUser);
   fh->Open((path).c_str(),O_RDONLY, 0600, m_env);
   Info cfi(0);
   long long off = cfi.Read(fh);

   std::vector<Info::AStat> statv;

   for (int i = 0; i <cfi.GetAccessCnt(); ++i ) {
      Info::AStat a;
      off += fh->Read(&a, off , sizeof(Info::AStat));
      statv.push_back(a);
   }

   int cntd = 0;
   for (int i = 0; i < cfi.GetSizeInBits(); ++i) if (cfi.TestBit(i)) cntd++;


   printf("version == %d, bufferSize %lld nBlocks %d nDownlaoded %d %s\n",cfi.GetVersion(), cfi.GetBufferSize(), cfi.GetSizeInBits() , cntd, (cfi.GetSizeInBits() == cntd) ? " complete" :"");

   if (m_verbose) {
      printf("printing %d blocks: \n", cfi.GetSizeInBits());
      for (int i = 0; i < cfi.GetSizeInBits(); ++i)
         printf("%c ", cfi.TestBit(i) ? 'x':'.');
      printf("\n");
   }

   for (int i=0; i < cfi.GetAccessCnt(); ++i)
   {
      printf("access %d >> ", i);
      Info::AStat& a = statv[i];
      char s[1000];
      struct tm * p = localtime(&a.DetachTime);
      strftime(s, 1000, "%c", p);
      printf("[%s], bytesDisk=%lld, bytesRAM=%lld, bytesMissed=%lld\n", s, a.BytesDisk, a.BytesRam, a.BytesMissed);
   }

   delete fh;
   printf("\n");
}

void Print::printDir(XrdOssDF* iOssDF, const std::string& path) 
{
   // printf("---------> print dir %s \n", path.c_str());
   char buff[256];
   int rdr;
   while ( (rdr = iOssDF->Readdir(&buff[0], 256)) >= 0)
   {
      if (strncmp("..", &buff[0], 2) && strncmp(".", &buff[0], 1)) {

         if (strlen(buff) == 0) {
            break; // end of readdir
         }
         std::string np = path + "/" + std::string(&buff[0]);
         if (isInfoFile(buff))
         {
            printFile(np);
         }
         else {
            XrdOssDF* dh = m_oss->newDir(m_ossUser);
            if (dh->Opendir(np.c_str(), m_env) >= 0) {
               printDir(dh, np);
            }
            delete dh; dh = 0;
         }
      }
   }
}


//______________________________________________________________________________

int main(int argc, char *argv[])
{ 
   static const char* usage = "Usage: pfc_print [-c config_file] [-v] path\n\n";
   bool verbose = false;
   const char* cfgn = 0;
  
   XrdOucEnv myEnv;

   XrdSysLogger log;
   XrdSysError err(&log);


   XrdOucStream Config(&err, getenv("XRDINSTANCE"), &myEnv, "=====> ");
   XrdOucArgs Spec(&err, "pfc_print: ",    "", 
                   "verbose",        1, "v",
                   "config",       1, "c",
                   (const char *)0);


   Spec.Set(argc-1, &argv[1]);
   char theOpt;

   while((theOpt = Spec.getopt()) != (char)-1)
   {
      switch(theOpt)
      {
         case 'c': {
            cfgn = Spec.getarg();
            int fd = open(cfgn, O_RDONLY, 0);
            Config.Attach(fd);
            break;
         }
         case 'v': {
            verbose = true;
            break;
         }
         default: {
            printf("%s", usage);
            exit(1);
         }
      }
   }


   // suppress oss init messages
   int efs = open("/dev/null",O_RDWR, 0); 
   XrdSysLogger ossLog(efs);
   XrdSysError ossErr(&ossLog, "print");
   XrdOss *oss; 
   XrdOfsConfigPI *ofsCfg = XrdOfsConfigPI::New(cfgn,&Config,&ossErr);
   bool ossSucc = ofsCfg->Load(XrdOfsConfigPI::theOssLib);
   if (!ossSucc) {
      printf("can't load oss\n");
      exit(1);
   }
   ofsCfg->Plugin(oss);

   const char* path = Spec.getarg();
   if (!path) {
      printf("%s", usage);
      exit(1);
   }

   // append oss.localroot if path starts with 'root://'
   if (!strncmp(&path[0], "root:/", 6)) {
      if (Config.FDNum() < 0) {
         printf("Configuration file not specified.\n");
         exit(1);
      }
      char *var;
      while((var = Config.GetFirstWord()))
      {
         // printf("var %s \n", var);
         if (!strncmp(var,"oss.localroot", strlen("oss.localroot")))
         {
            std::string tmp = Config.GetWord();
            tmp += &path[6];
            // printf("Absolute path %s \n", tmp.c_str());  
            XrdFileCache::Print p(oss, verbose, tmp.c_str());
         }
      }
   }
   else {
       XrdFileCache::Print p(oss, verbose, path);
   }
}


