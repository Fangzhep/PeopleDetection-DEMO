#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
#include <iostream>

// Platform detection
#if defined(__linux__)
#include <linux/i2c-dev.h>
#include <fcntl.h>
#include <unistd.h>
#else
// Windows dummy I2C implementation
#define O_RDWR 0
#define I2C_SLAVE 0
int open(const char*, int) { return 1; }
int ioctl(int, int, int) { return 0; }
int close(int) { return 0; }
ssize_t write(int, const void*, size_t) { return 0; }
#endif

using namespace cv;
using namespace dnn;
using namespace std;

// Initialize the parameters
float confThreshold = 0.5; // Confidence threshold
float nmsThreshold = 0.4;  // Non-maximum suppression threshold
int inpWidth = 300;        // Width of network's input image
int inpHeight = 300;       // Height of network's input image
vector<string> classes;

// I2C Configuration
const char *I2C_DEVICE = "/dev/i2c-1";
const int STM32_ADDRESS = 0x08; // STM32 I2C address
int i2cFile;

// Remove the bounding boxes with low confidence using non-maxima suppression
void postprocess(Mat& frame, const vector<Mat>& outs);

// Draw the predicted bounding box and send I2C message if person detected
void drawPred(int classId, float conf, int left, int top, int right, int bottom, Mat& frame);

// Get the names of the output layers
vector<String> getOutputsNames(const Net& net);

// Function to send I2C message
void sendI2CMessage(const string& message) {
#if defined(__linux__)
    if (write(i2cFile, message.c_str(), message.length()) != message.length()) {
        cerr << "Failed to write to the I2C bus" << endl;
    }
#else
    cout << "[I2C Simulation] Sending message: " << message << endl;
#endif
}

int main(int argc, char** argv)
{
    // Check OpenCV version
    cout << "OpenCV version: " << CV_VERSION << endl;

    // Initialize I2C
#if defined(__linux__)
    if ((i2cFile = open(I2C_DEVICE, O_RDWR)) < 0) {
        cerr << "Failed to open the I2C bus" << endl;
        return -1;
    }
    if (ioctl(i2cFile, I2C_SLAVE, STM32_ADDRESS) < 0) {
        cerr << "Failed to acquire bus access and/or talk to slave" << endl;
        return -1;
    }
#else
    cout << "[I2C Simulation] Initializing I2C interface" << endl;
    i2cFile = 1;
#endif

    // Load names of classes
    string classesFile = "coco.names";
    ifstream ifs(classesFile.c_str());
    string line;
    while (getline(ifs, line)) classes.push_back(line);
    
    // Give the configuration and weight files for the model
    String modelConfiguration = "deploy.prototxt";
    String modelWeights = "mobilenet_iter_73000.caffemodel";
    
    // Load the network
    Net net = readNetFromCaffe(modelConfiguration, modelWeights);
    net.setPreferableBackend(DNN_BACKEND_OPENCV);
    net.setPreferableTarget(DNN_TARGET_CPU);

    // Open a video file or an image file or a camera stream
    string str, outputFile;
    VideoCapture cap;
    VideoWriter video;
    Mat frame, blob;
    
    try {
        cap.open(0); // Open the default camera
    }
    catch(...) {
        cout << "Could not open camera" << endl;
        return -1;
    }

    while (waitKey(1) < 0)
    {
        // Get frame from the video
        cap >> frame;
        
        // Stop the program if reached end of video
        if (frame.empty()) {
            cout << "Done processing !!!" << endl;
            break;
        }
        
        // Create a 4D blob from a frame
        blobFromImage(frame, blob, 1/127.5, Size(inpWidth, inpHeight), Scalar(127.5, 127.5, 127.5), true, false);
        
        // Sets the input to the network
        net.setInput(blob);
        
        // Runs the forward pass to get output of the output layers
        vector<Mat> outs;
        net.forward(outs, getOutputsNames(net));
        
        // Remove the bounding boxes with low confidence
        postprocess(frame, outs);
        
        // Put efficiency information
        vector<double> layersTimes;
        double freq = getTickFrequency() / 1000;
        double t = net.getPerfProfile(layersTimes) / freq;
        string label = format("Inference time: %.2f ms", t);
        putText(frame, label, Point(0, 15), FONT_HERSHEY_SIMPLEX, 0.5, Scalar(0, 0, 255));
        
        // Write the frame with the detection boxes
        imshow("People Detection", frame);
    }
    
    cap.release();
    close(i2cFile); // Close I2C connection
    return 0;
}

// Remove the bounding boxes with low confidence using non-maxima suppression
void postprocess(Mat& frame, const vector<Mat>& outs)
{
    vector<int> classIds;
    vector<float> confidences;
    vector<Rect> boxes;
    
    for (size_t i = 0; i < outs.size(); ++i)
    {
        // Scan through all the bounding boxes output from the network and keep only the
        // ones with high confidence scores. Assign the box's class label as the class
        // with the highest score for the box.
        float* data = (float*)outs[i].data;
        for (int j = 0; j < outs[i].rows; ++j, data += outs[i].cols)
        {
            Mat scores = outs[i].row(j).colRange(5, outs[i].cols);
            Point classIdPoint;
            double confidence;
            // Get the value and location of the maximum score
            minMaxLoc(scores, 0, &confidence, 0, &classIdPoint);
            if (confidence > confThreshold)
            {
                int centerX = (int)(data[0] * frame.cols);
                int centerY = (int)(data[1] * frame.rows);
                int width = (int)(data[2] * frame.cols);
                int height = (int)(data[3] * frame.rows);
                int left = centerX - width / 2;
                int top = centerY - height / 2;
                
                classIds.push_back(classIdPoint.x);
                confidences.push_back((float)confidence);
                boxes.push_back(Rect(left, top, width, height));
            }
        }
    }
    
    // Perform non maximum suppression to eliminate redundant overlapping boxes with
    // lower confidences
    vector<int> indices;
    NMSBoxes(boxes, confidences, confThreshold, nmsThreshold, indices);
    for (size_t i = 0; i < indices.size(); ++i)
    {
        int idx = indices[i];
        Rect box = boxes[idx];
        drawPred(classIds[idx], confidences[idx], box.x, box.y,
                 box.x + box.width, box.y + box.height, frame);
    }
}

// Draw the predicted bounding box and send I2C message if person detected
void drawPred(int classId, float conf, int left, int top, int right, int bottom, Mat& frame)
{
    // Only draw person class (classId 0)
    if (classId == 0) {
        // Draw a rectangle displaying the bounding box
        rectangle(frame, Point(left, top), Point(right, bottom), Scalar(255, 178, 50), 3);
        
        // Get the label for the class name and its confidence
        string label = format("%.2f", conf);
        if (!classes.empty())
        {
            CV_Assert(classId < (int)classes.size());
            label = classes[classId] + ":" + label;
        }
        
        // Display the label at the top of the bounding box
        int baseLine;
        Size labelSize = getTextSize(label, FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseLine);
        top = max(top, labelSize.height);
        rectangle(frame, Point(left, top - round(1.5*labelSize.height)),
                  Point(left + round(1.5*labelSize.width), top + baseLine), Scalar(255, 255, 255), FILLED);
        putText(frame, label, Point(left, top), FONT_HERSHEY_SIMPLEX, 0.75, Scalar(0,0,0),1);
        
        // Send I2C message when person is detected
        string message = "PERSON_DETECTED";
        sendI2CMessage(message);
    }
}

// Get the names of the output layers
vector<String> getOutputsNames(const Net& net)
{
    static vector<String> names;
    if (names.empty())
    {
        // Get the indices of the output layers, i.e. the layers with unconnected outputs
        vector<int> outLayers = net.getUnconnectedOutLayers();
        
        // Get the names of all the layers in the network
        vector<String> layersNames = net.getLayerNames();
        
        // Get the names of the output layers in names
        names.resize(outLayers.size());
        for (size_t i = 0; i < outLayers.size(); ++i)
            names[i] = layersNames[outLayers[i] - 1];
    }
    return names;
}
