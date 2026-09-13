# CommonLibSSE-NG Plugin Template

This is a basic plugin template using CommonLibSSE-NG.

### Requirements
* [XMake](https://xmake.io) [2.8.2+]
* C++23 Compiler (MSVC, Clang-CL)

## Getting Started
```bat
git clone --recurse-submodules https://github.com/Teijirr/commonlibsse-ng-template/
cd commonlibsse-ng-template
```
Or clone with your IDE and run this command:
```
git submodule update --init --recursive
```

### Change project name and author in xmake.lua

### Update commonlibng : 
```
cd lib/commonlibsse-ng
git fetch --tags
git checkout ng
git pull
```

### Build
To build the vs project, run the following command:
```bat
xmake project -k vsxmake
```

### Build Output (Optional)

#### Mod Directory
Set one of or both of the following environment variables:

- Path to a Skyrim install folder: `XSE_TES5_GAME_PATH`

- Path to a Mod Manager mods folder: `XSE_TES5_MODS_PATH`

#### Mod Resources
Copy resources like ini files, Bethesda plugins, etc. with the custom rule `copy_resource`
