IF NOT EXIST "build" (
	ECHO create build folder
	mkdir build
)
cd build
IF NOT EXIST "D:\buildtools\swigwin-3.0.12\swig.exe" (
	ECHO swig.exe not exist or not at D:/buildtools/swigwin-3.0.12/swig.exe
	ECHO please download and put it to the suggested location
)

cmake .. -A x64 -DCMAKE_BUILD_TYPE=Release -DSWIG_EXECUTABLE=C:\swigwin-3.0.12\swig.exe -DPYTHON_EXECUTABLE=C:\Anaconda\envs\py35\python.exe -DPYTHON_LIBRARIES=C:/Anaconda/envs/py35/libs/python35.lib -Dtensorflow_BUILD_CC_TESTS=OFF

MSBuild /p:Configuration=Release tf_label_image_example.vcxproj

cd ..