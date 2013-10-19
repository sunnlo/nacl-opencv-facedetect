#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <stdio.h>
#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/module.h"
#include "ppapi/cpp/var.h"
extern "C" {
#include "jpeglib.h"
}

#include "opencv2/core/core.hpp"
#include "opencv2/imgproc/imgproc_c.h"
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/contrib/contrib.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/objdetect/objdetect.hpp"

#include "naclmounts/base/Mount.h"
#include "naclmounts/base/MountManager.h"
#include "naclmounts/memory/MemMount.h"

#include "test.h"
#include "jpeg_mem_src.h"
#include "geturl_handler.h"

Mat testInstance::norm_0_255(InputArray _src){
    Mat src = _src.getMat();
    // Create and return normalized image
    Mat dst;
    switch(src.channels()){
        case 1:
            cv::normalize(_src, dst, 0, 255, NORM_MINMAX, CV_8UC1);
            break;
        case 3:
            cv::normalize(_src, dst, 0, 255, NORM_MINMAX, CV_8UC3);
            break;
        default:
            src.copyTo(dst);
            break;
    }
    return dst;
}

void testInstance::read_csv(const string& filename, vector<Mat>& images, vector<int>& labels, char separator = ';') {
    std::ifstream file(filename.c_str(), ifstream::in);
    if (!file) {
        string error_message = "No valid input file was given, please check the given filename.";
        CV_Error(CV_StsBadArg, error_message);
    }
    string line, path, classlabel;
    while (getline(file, line)) {
        stringstream liness(line);
        getline(liness, path, separator);
        getline(liness, classlabel);
        if(!path.empty() && !classlabel.empty()) {
            images.push_back(imread(path, 0));
            labels.push_back(atoi(classlabel.c_str()));
        }
    }
}

/* Read first AJAX call to and parse the string to get the data (src of the image, width, height) */
void testInstance::HandleMessage(const pp::Var& var_message) {
	// download the url in the var_message
	stringstream convert;
	string data = var_message.AsString();
	convert << data.substr(0, data.find_first_of('|'));
	convert >> this->width;
	stringstream convert2;
	convert2 << data.substr(data.find_first_of('|')+1, data.find_last_of('|')-data.find_first_of('|')-1);
	convert2 >> this->height;
	this->url = data.substr(data.find_last_of('|')+1, data.length());
	this->handler = GetURLHandler::Create(this, this->url);
	this->handler->file = GetURLHandler::URL_IMAGE; // mark the handler saying we are getting the image
	if(this->handler != NULL){
		this->handler->Start();
	}
}

void testInstance::HandleImage(const string& message){
        struct jpeg_decompress_struct cinfo;
        struct jpeg_error_mgr jerr;

        // Initialize the JPEG decompression object with default error handling.
        cinfo.err = jpeg_std_error(&jerr);
        jpeg_create_decompress(&cinfo);

        // Specify data source for decompression
        jpeg_mem_src(&cinfo, (unsigned char*)message.c_str(),(unsigned long)message.size());

        // Read file header, set default decompression parameters
        (void) jpeg_read_header(&cinfo, TRUE);


        // Start decompressor
        (void) jpeg_start_decompress(&cinfo);

	// Read the image row by row
	this->frame = cvCreateImage(cvSize(cinfo.output_width,cinfo.output_height),
                                        IPL_DEPTH_8U, cinfo.output_components);
        JSAMPARRAY imageBuffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE, 
                                     cinfo.output_width*cinfo.output_components, 1);
        for(unsigned int y=0;y< cinfo.output_height;y++){
                jpeg_read_scanlines(&cinfo, imageBuffer, 1);
                uint8_t* dstRow = (uint8_t*) this->frame->imageData + this->frame->widthStep*y;
                memcpy(dstRow, imageBuffer[0], cinfo.output_width*cinfo.output_components);
        }

	// If the image is 3 channel convert it into BGR (OpenCV standard)
        if(cinfo.output_components == 3){
                cvCvtColor(this->frame, this->frame, CV_RGB2BGR);
        }

	// if XML classifier has already been loaded, skip it
	if(this->classifierCreated == true){
		this->RecognizeFace();
	} else {
		// get xml file
		this->handler = GetURLHandler::Create(this, "haarcascade_frontalface_alt.xml");
		this->handler->file = GetURLHandler::URL_XML;
		if(this->handler != NULL){
			this->handler->Start();
		}
	}
}

void testInstance::HandleXml(const string& content){
	const char* cascadefilename = "/haarcascade_frontalface_alt.xml";
	this->CreateMemFile(content, cascadefilename);
	this->RecognizeFace();
}

void testInstance::CreateMemFile(const string& content, const string& filename){
	// stream message for debugging
	stringstream ss;
	// test filesys module of nacl-mount to add xml file
	MountManager *mm = MountManager::MMInstance();
	KernelProxy* kp = MountManager::MMInstance()->kp();
	MemMount *mnt = new MemMount();
	mm->AddMount(mnt, "/");
	int fd = kp->open(filename, O_WRONLY | O_CREAT, 0644);
	if(fd == -1){
		ss << "--(!) Error creating cascade classifier";
		this->PostMessage(pp::Var(ss.str()));
		return;
	}
	kp->write(fd, content.c_str(), content.size());
	kp->close(fd);
	this->classifierCreated = true;

	// TEST LOADING THE XML FILE FROM THE MEM STORAGE//
	// load xml face cascade to detect face
    fprintf(stderr, "[NACL Log]: Printing error to stderr!?!\n");
	FileStorage fs(filename, FileStorage::READ);
    fprintf(stderr, "[NACL Log]: Opening the file using FileStorage::open?!?!\n");
    bool ok = fs.open(filename, FileStorage::READ);
    fprintf(stderr, "[NACL Log]: Finished this fs.open thing!\n");
    fprintf(stderr, "[NACL Log]: Finished this fs.open thing!\n");
    if(!fs.isOpened())
        printf("Failed opening cascade..\n");
    fs.getFirstTopLevelNode();
    //int size1 = (int)fs["test"];
    fs.release();
	//FileNode t = fs.getFirstTopLevelNode();
    fprintf(stderr, "[NACL Log]: Finished fetching the first node!\n");
}

void testInstance::RecognizeFace(){
	// stream message for debugging
	stringstream ss;
	const char* cascadeFilename = "/haarcascade_frontalface_alt.xml";
	CascadeClassifier face_cascade;
	face_cascade.load(cascadeFilename);
	if( !face_cascade.load(cascadeFilename) ){ 
		ss << "--(!)Error loading cascade classifier";
		this->PostMessage(pp::Var(ss.str()));
		return;
	}

        std::vector<Rect> faces;
        Mat frame_gray;

        cvtColor( Mat(this->frame), frame_gray, CV_BGR2GRAY );
        equalizeHist( frame_gray, frame_gray );

        //-- Detect faces
        face_cascade.detectMultiScale( frame_gray, faces, 1.1, 2, 0|CV_HAAR_SCALE_IMAGE, Size(30, 30) );

        for( unsigned int i = 0; i < faces.size(); i++ )
        {
		ss << "Face #" << i << " x=" << faces[i].x << " width=" << faces[i].width 
			<< " y=" << faces[i].y << " height=" << faces[i].height << "\n";
                //Point center( faces[i].x + faces[i].width*0.5, faces[i].y + faces[i].height*0.5 );
        }

	this->PostMessage(pp::Var(ss.str()));
}

/// The Module class.  The browser calls the CreateInstance() method to create
/// an instance of your NaCl module on the web page.  The browser creates a new
/// instance for each <embed> tag with type="application/x-nacl".
class testModule : public pp::Module {
    public:
        testModule() : pp::Module() {}
        virtual ~testModule() {}

        /// Create and return a testInstance object.
        /// @param[in] instance The browser-side instance.
        /// @return the plugin-side instance.
        virtual pp::Instance* CreateInstance(PP_Instance instance) {
            return new testInstance(instance);
        }
};

namespace pp {
    /// Factory function called by the browser when the module is first loaded.
    /// The browser keeps a singleton of this module.  It calls the
    /// CreateInstance() method on the object you return to make instances.  There
    /// is one instance per <embed> tag on the page.  This is the main binding
    /// point for your NaCl module with the browser.
    Module* CreateModule() {
        return new testModule();
    }
}  // namespace pp