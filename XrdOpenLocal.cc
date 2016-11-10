/********************************************************************************
 *    Copyright (C) 2014 GSI Helmholtzzentrum fuer Schwerionenforschung GmbH    *
 *                                                                              *
 *              This software is distributed under the terms of the             * 
 *         GNU Lesser General Public Licence version 3 (LGPL) version 3,        *  
 *                  copied verbatim in the file "LICENSE"                       *
 ********************************************************************************/

#include "XrdOpenLocal.hh"
#include <exception>
#include <cstdlib>
#include <string>
#include <utility>
#include "XrdCl/XrdClUtils.hh"
#include <assert.h>
using namespace XrdCl;
XrdVERSIONINFO(XrdClGetPlugIn, OpenLocal);

namespace OpenLocal {
class OpenLocalFile : public XrdCl::FilePlugIn {


private:

	///@swapLocalMap a map for rewrite some server specific URL to using an local file
	// e.g. root://xrd-manager.your.site to /your/filesystem/xrdmanager (specified in the xrootd plugin config file)
	static std::map<std::string,std::string> swapLocalMap;
	///@proxyPrefix The prefix that will be added to any root query that cannot use local available files
	///@file file for local access
	fstream* file;
std::string path;
public:
	static void printInfo() {
		XrdCl::Log *log= XrdCl::DefaultEnv::GetLog();
		log->Debug(1,"OpenLocalFile::printInfo");
		log->Debug(1,"Swap to Local Map:");
		for(auto i : swapLocalMap) {
			stringstream msg ;
			msg<<"\""<<i.first<<"\" to \""<<i.second<<"\""<<std::endl;
			log->Debug(1,msg.str().c_str());
		}
	}

	static void setSwapLocalMap(std::pair<std::string,std::string>toadd) {
		swapLocalMap.insert(toadd);
	}

	static void parseIntoLocalMap(std::string configline) {
		std::istringstream ss(configline);
		std::string token;
		while(std::getline(ss,token,';')) {
			std::istringstream sub(token);
			std::string lpath;
			std::string rpath;
			std::getline(sub,lpath,'|');
			std::getline(sub,rpath,'|');
			setSwapLocalMap(std::make_pair(lpath,rpath));
		}
	}

	std::string  getLocalAdressMap( std::string servername) {
		auto addr=swapLocalMap.find(servername);
		if(addr==swapLocalMap.end()) {
			return "NotInside";
		} else {
			return addr->second;
		}
	}

	std::string LocalPath(std::string url) {
		XrdCl::Log *log= XrdCl::DefaultEnv::GetLog();
		XrdCl::URL xUrl(url);
		string path=xUrl.GetPath();
		string servername=xUrl.GetHostName();
		std::stringstream out;

		out << "OpenLocal::setting  url:\"" <<url<<"\"";
		if(getLocalAdressMap(servername).compare("NotInside")!=0) {
			std::string   lpath=getLocalAdressMap(servername);
			lpath.append(path);
			this->path=lpath;

			out<<" to: \""<<lpath<<"\""<<std::endl;
			log->Debug(1,out.str().c_str());

			return lpath;
		}
	}

	//Constructor
	OpenLocalFile(std::string x){ //declare that xfile shall not recursively use plugins
		file=new fstream();

	}

	//Destructor
	~OpenLocalFile() {
		file->close();
	
	}
	//Open()
	virtual XRootDStatus Open( const std::string &url,
	                           OpenFlags::Flags   flags,
	                           Access::Mode       mode,
	                           ResponseHandler   *handler,
	                           uint16_t           timeout ) {
		XrdCl::Log *log=XrdCl::DefaultEnv::GetLog();
		XRootDStatus* ret_st;
		std::string newurl= LocalPath(url);
		log->Debug(1,"OpenLocalFile::Open");
			file->open(newurl.c_str(),std::ios::in  | std::ios::out| std::ios::app );
			if(file->fail()) {
				ret_st=new XRootDStatus( XrdCl::stError,XrdCl::errOSError, 1,"file could not be opened");
				handler->HandleResponse(ret_st,0);
				return *ret_st;
			}
			ret_st=new XRootDStatus(XrdCl::stOK,0,0,"");
			handler->HandleResponse(ret_st,0);
			return  *ret_st;
		}

	virtual XRootDStatus Close(ResponseHandler *handler,uint16_t timeout) {
		XrdCl::Log *log=XrdCl::DefaultEnv::GetLog();
			file->close();
			XRootDStatus* ret_st=new XRootDStatus(XrdCl::stOK,0,0,"");
			handler->HandleResponse(ret_st,0);
			return  XRootDStatus(XrdCl::stOK,0,0,"");


	}

	virtual bool IsOpen()  const    {
	 return file->is_open();
	}
	virtual XRootDStatus Stat(bool force,ResponseHandler *handler,uint16_t timeout) {
		XrdCl::Log *log=XrdCl::DefaultEnv::GetLog();
		log->Debug(1,"OpenLocalFile::Stat");

			if(file->is_open() && file!=NULL) {
				struct stat s;
				stat(path.c_str(),&s);
				StatInfo* sinfo = new StatInfo();
				std::ostringstream data;
				data<<s.st_dev <<" "<< s.st_size <<" "<<s.st_mode<<" "<<s.st_mtime ;
				std::string output ="OpenLocal::Stat, stats are: (_dev,_size,_mode,_mtime) ";
				output.append(data.str().c_str());
				log->Debug(1,output.c_str());

				if(!sinfo->ParseServerResponse(data.str().c_str())) {
					delete sinfo;
					return XRootDStatus(XrdCl::stError, errDataError);
				} else {
					XRootDStatus* ret_st = new XRootDStatus(XrdCl::stOK, 0, 0, "");
					AnyObject* obj = new AnyObject();
					obj->Set(sinfo);
					handler->HandleResponse(ret_st, obj);
					log->Debug( 1, "OpenLocalFile::Stat returning stat structure");
					return XRootDStatus( XrdCl::stOK,0,0,"");
				}


			} else {
				log->Debug(1,"OpenLocalFile::Stat::Error No file opened");
				return XRootDStatus( XrdCl::stError,XrdCl::errOSError,-1,"no file opened error");
			}
				return XRootDStatus( XrdCl::stError,XrdCl::errOSError,-1,"no file opened error");
	}

	virtual XRootDStatus Read(uint64_t offset,uint32_t length,
	                          void  *buffer,XrdCl::ResponseHandler *handler,
	                          uint16_t timeout ) {
		XrdCl::Log *log=XrdCl::DefaultEnv::GetLog();
		log->Debug(1,"OpenLocal::Read");
			file->seekp(offset);
			file->read( (char*)buffer,length);
			XRootDStatus* ret_st=new XRootDStatus(XrdCl::stOK,0,0,"");
			ChunkInfo* chunkInfo=new ChunkInfo(offset,length,buffer );
			AnyObject* obj=new AnyObject();
			obj->Set(chunkInfo);
			handler->HandleResponse(ret_st,obj);
			return  XRootDStatus(XrdCl::stOK,0,0,"");
	}

	XRootDStatus Write( uint64_t         offset,
	                    uint32_t         size,
	                    const void      *buffer,
	                    ResponseHandler *handler,
	                    uint16_t         timeout = 0 ) {
		XrdCl::Log *log=XrdCl::DefaultEnv::GetLog();
		log->Debug(1,"OpenLocalFile::Write");
		file->seekg(offset);
		file->write((char*)buffer,size);
		XRootDStatus* ret_st=new XRootDStatus(XrdCl::stOK,0,0,"");
		handler->HandleResponse(ret_st,0);
		return  XRootDStatus(XrdCl::stOK,0,0,"");

	}
};
std::map<std::string,std::string> OpenLocalFile::swapLocalMap ;

class OpenLocalFs : public XrdCl::FileSystemPlugIn {
private:
public:
	XrdCl::FileSystem fs;
	//Constructor
	OpenLocalFs(std::string url):fs(url,false) {
	}
	//Destructor
	~OpenLocalFs() {
	}
};
};
namespace XrdOpenLocalFactory {
XOLFactory::XOLFactory( const std::map<std::string, std::string> &config ) :
	XrdCl::PlugInFactory() {
	XrdCl::Log *log = DefaultEnv::GetLog();
	log->Debug( 1, "XrdOpenLocalFactory::Constructor" );
	if(config.find("redirectlocal")!=config.end())OpenLocal::OpenLocalFile::parseIntoLocalMap(config.find("redirectlocal")->second);
	else{throw std::runtime_error("Config file does not contain any values for the redirectlocal key");}
	OpenLocal::OpenLocalFile::printInfo();
}

XOLFactory::~XOLFactory() {
}

XrdCl::FilePlugIn * XOLFactory::CreateFile( const std::string &url ) {
	return static_cast<XrdCl::FilePlugIn *> (new OpenLocal::OpenLocalFile(url)) ;
}

XrdCl::FileSystemPlugIn * XOLFactory::CreateFileSystem(const std::string &url) {
	return static_cast<XrdCl::FileSystemPlugIn *> (new OpenLocal::OpenLocalFs(url)) ;

}
}
extern "C" {
	void *XrdClGetPlugIn(const void *arg) {
		const std::map<std::string, std::string> &pconfig = *static_cast <const std::map<std::string, std::string> *>(arg);
		void * plug= new  XrdOpenLocalFactory::XOLFactory(pconfig);
		return plug;
	}
}