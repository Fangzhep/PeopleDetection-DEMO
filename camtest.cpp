#include <opencv2/opencv.hpp>
#include <iostream>

int main() {
    // Try to open the default camera (usually /dev/video0 on Raspberry Pi)
    cv::VideoCapture cap(0);
    
    if(!cap.isOpened()) {
        std::cerr << "ERROR: Could not open camera" << std::endl;
        return -1;
    }

    std::cout << "Camera opened successfully!" << std::endl;
    
    // Release the camera
    cap.release();
    return 0;
}
