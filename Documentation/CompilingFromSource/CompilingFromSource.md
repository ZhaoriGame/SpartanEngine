# Compiling from source

### Setting up the environment
The engine currently uses DirectX 11 as a rendering backend and some of the latest C++ features. As a result we first have to make sure that we set up the right environment for it. Below we can see it's few dependencies and how to address them.
##### DirectX End-User Runtimes: Download by clicking [here](https://www.microsoft.com/en-us/download/details.aspx?id=8109) and install
![Screenshot](https://raw.githubusercontent.com/PanosK92/Directus3D/master/Documentation/CompilingFromSource/DirectX.png)

##### Visual C++ 2017 (x64) runtime package: Download by clicking [here](https://go.microsoft.com/fwlink/?LinkId=746572) and install
![Screenshot](https://raw.githubusercontent.com/PanosK92/Directus3D/master/Documentation/CompilingFromSource/Visual%20C%2B%2B.png)

### Compiling the Runtime and the Editor
At this point we have taken care of all the environment dependencies and we are ready to start building.

##### Generating Visual Studio 2017 project files and building the runtime
1. We click and run **"Generate_VS17_Project.bat"** in order for a Visual Studio solution to be generated.
![Screenshot](https://raw.githubusercontent.com/PanosK92/Directus3D/master/Documentation/CompilingFromSource/GenerateVS.png)
2. We then open the Visual Studio solution file named **"Directus.sln"**
![Screenshot](https://raw.githubusercontent.com/PanosK92/Directus3D/master/Documentation/CompilingFromSource/GenerateVS2.png)
3. Next, we switch the solution configuration to **"Release"** and build the entire solution. This will generate **"Editor.exe"** at **"Binaries\Release"**.
![Screenshot](https://raw.githubusercontent.com/PanosK92/Directus3D/master/Documentation/CompilingFromSource/GenerateVS3.png)

### Note
- The pre-compiled libraries (**ThirdParty\mvsc141_x64**) are provided for convenience. If you get any linking errors due to version incompatibilities, it is advised that you download and compile the dependency.