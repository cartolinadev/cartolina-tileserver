<p align="center"><img width="320" alt="cartolina-tileserver" src="https://raw.githubusercontent.com/cartolinadev/assets/refs/heads/main/brand/cartolina-logo.png"></p>


# `cartolina-tileserver`

**`cartolina-tileserver`** is a 3D geospatial data streaming server. It's the 
primary backend component of [cartolina](http://cartolina.dev/), an experimental 
software stack for web-based 3D terrain cartography.

The tile server provides on-the-fly access to an array of raster and vector data
formats and serves those data in formats optimized for 3D streaming and 
rendering. 

`cartolina-tileserver` is a fork of — and a replacement for — [vts-mapproxy](https://github.com/melowntech/vts-mapproxy/),
which was authored and developed by Melown Technologies/Leica Geosystems 
in 2017-2023 and which is now officially discontinued.  

## Features

* dynamic TIN (terrain mesh) generation from DEMs (digital elevation models)
* on-the-fly CRS conversion for *massive* raster datasets
* on-the-fly configurable shaded relief from DEMs
* DEM-based normal maps
* specular reflection maps based on a land-cover classification 
* bump maps based on satellite or aerial imagery
* WMTS service for raster datasets 
* introspection capabilities (web-based, interactive resource directories with embedded viewers)
* [CesiumJS](https://cesiumjs.org) terrain provisioning (possibly outdated)

## What's different from the legacy vts-mapproxy

Apart from minor fixes and refactoring, the main difference is in the new 
functionality:

* there are three new TMS generators (**tms-gdaldem**, **tms-normalmap** and 
**tms-specularmap**) 

* terrain TINs (or surfaces in cartolina terminology) produced by the 
**surface-\*** now come with normal maps, which is in line with the independent
lighting model that cartolina-js relies on. 

By and large `cartolina-tileserver` is a functional superset rather than a divergent fork
of the original mapproxy, so it can be readily used as a replacement. Whatever 
worked with the old vts-mapproxy should still work with `cartolina-tileserver`. 


## Documentation

There is currently no original documentation for `cartolina-tileserver`, but you
can rely on the [legacy documentation of vts-mapproxy](https://web.archive.org/web/20230206094802/https://vts-geospatial.org/reference/server/mapproxy/index.html#mapproxy), which is still
relevant.

If you want to use the new generators you need to figure things out [from the source code](mapproxy/src/mapproxy/definition/tms.hpp) at the 
moment. But if you have the technological dexterity to run the tile server
you should not find that very difficult.

You don't really need to do anything to use the TIN normal maps generated from
DEMs: these are generated from the default configuration including the metadata
that allow `cartolina-js` to find them.

Authoritative resource-definition documentation is available in a separate [document](docs/resources.md).


## Install

I strongly encourage you to use the binary package distribution if you want to run
`cartolina-tileserver`.


To use the Debian APT package repository on Ubuntu 22.04 (Jammy Jellyfish) or 
compatible systems, create an APT source file with your favorite editor

```bash
sudo nano /etc/apt/sources.list.d/tspl-re.sources
```

add exactly the following contents

```bash
Types: deb
URIs: https://debian.tspl.re/debian/
Suites: jammy
Components: main
Signed-By:
 -----BEGIN PGP PUBLIC KEY BLOCK-----
 . 
 mQINBGbKQU4BEAC+8vpH6bUqF0FycVZmOC/iL3OY5f0RMt3PJoG8NLMPAToDdffG
 Zav2Wh/02a131sLcqil8COOOIH/A9apryyYSeOKbHgGQOdxuJbxgMtiQe/Nr3OmM
 zyAMeasXjw0y98vqhsk/NIClZ01IVKb7vZlSCyVEVHxAAlvGnsfvcOR90BLmVopq
 wHHT6BViuZxoMfou4DVghVYCgzksW0nZH2bO43y1hbZeS4qUHgWpgRxXttpt4CbP
 ztEn20zMAmhBPnPVTM5714ixRMV2fibE0hA7S41lxseMtOvSVvTUQ+aVvCA4nMrf
 2/jxTpL9riGybo6fvlEjPtdU9X00rUtZl37rW+9hNqqSN7NXn7IBctm5NTnKgaFk
 qcd/mdb8RXcJbhI9EeO1JZ2/A53ecByA0qfuilVDeIyXJOJH8hBKLaKNm32LpWLw
 UMyDLaRLImcpgYhiLtOd9AsDGOLTotoDymwIzPAQeV8ONtBzkfBz5F+ydSKKS1uH
 ITIueFYJFsQRhuw0Qrpj4k+a0ZRjVKqbaF2hhBVmgZbN8QkYWtuR7MSfnTnS5sC6
 oSORcxcq03w0Mbks1tLQXP9jB5EsK+G0/YAIR04sueZUBavZq67a9A2xRyaVEfVu
 PNc2Ia29FcSp47Ukl4IxnplLWoq3SL3cX3gsGgSr2QlRrXL544yt41ipmQARAQAB
 tCJ0c3BsLnJlIHJlcG9zaXRvcnkgPHJlcG9zQHRzcGwucmU+iQJOBBMBCgA4FiEE
 sHfLmRPHyjnbVvgjhZV/KiSxDkcFAmbKQU4CGwMFCwkIBwIGFQoJCAsCBBYCAwEC
 HgECF4AACgkQhZV/KiSxDkeWdg/9E4n9HWfhW1+Muqnip/G+afLaKG5BJ+Y7ChlY
 1D9eUHSQk4mCKNzibYt9uzOhcTffwj7YjWvShZ3FVf8wx+M51MLdLTsKf1U1iBPA
 ErBrPY2p8+J8/eIZKjA8qSEHJBLwZ4OLyUtD4NTtg+ngbWRVfEMxrYYKawpF24Uy
 HJSfVz4KRzJvKXWAMVRW1uBJUGkct0Ov8CnFFN3CsJ5JQGFerjceYJH+S9vSexvo
 9tKPQCCAAEIpZjsn+bLYi0miyurd//dcWuOSsaSsrGJhxfr/1h0fVvnhIWAEW/pn
 sIPwCn7s1/e5IH5qa8U5q9slhglF1Wy4tJadD1HYuJGM9ybhxju0HapiLlsmw4Gl
 rJ3TsvdMBiqxeVXarARKb+W7/sonPONivO6sF20f4YRgxg+BZM6rWzmEkf7+VL12
 U1LXI5BypVUwato8fd+2/nazYP1wXs+2JznSRXu/7ubuwlo8rCg8Op3mSsAoyqHp
 QFdTnWr6U9DkEQYlke7ttTvtBY0vjAmQjn9FKUWuwS7dw1m8HUFdsByDZ83aihx8
 +PIoKoJfz2tqAwYuuXroM4NpIXaOUthBI9WJwvY77s9mu5KGpAchnss0fSd6wlRj
 2Wf6+K/FRWIFngy2cG6UGodsW2UJL3P2/rew1UXDpTAgvdLv1joi4zrDyIJ1O94J
 vhynVrs=
 =oL7w
 -----END PGP PUBLIC KEY BLOCK-----
```
   
and do

```bash
$ sudo apt update
```

Then, install the `vts-backend` metapackage. Apart from installing 
`cartolina-tileserver` itself, it takes care of the complex task of creating 
a mapproxy configuration file and configuring nginx reverse proxy for your 
tileserver.

```bash
sudo apt install vts-backend 
```

Now you can access the `cartolina-tileserver` introspection API by pointing
your web browser to

```
http://localhost:8070/mapproxy
```

If you prefer to do the preceding step manually, you can install just the 
tileserver:

```bash
sudo apt install cartolina-tileserver 
```

## Configuring and running the tile server

If you used the `vts-backend` metapackage, you can find the master tile 
server configuration file in `/etc/vts/mapproxy.conf`.

You can start, stop and restart the tileserver using the standard systemctl commands

```bash
$ sudo service vts-backend.mapproxy <start/restart/stop>
```

The tile server log file is located at '/var/log/vts/mapproxy.log'.

If you have not used the metapackage, you need to configure your server and 
set up the reverse proxy [manually](#configuring-and-running-your-own-build).

 
## Build from source

In theory, you need just 2 steps to build `cartolina-tileserver`: `git clone` 
the source code from the repository and `make` it. 

The reality is more complex. There will be undocumented unpackaged dependencies that you 
will run into and you are on your own resolving these. They relate to 
functionality you will not need, so you can  be pretty aggressive about resolving 
the compile-time dependency issues. Still, compiling from source is currently 
not as easy as it should be.

Here is the original HOWTO.

### Dependencies

#### Basic deps

Make sure you have `cmake` and `g++` installed:

```
sudo apt-get update
sudo apt-get install cmake g++
```

#### vts-geospatial dependencies

You need at least [vts-registry](https://github.com/melowntech/vts-registry) downloaded
and installed in your system. Please refer to related
[README.md](https://github.com/melowntech/vts-registry/blob/master/README.md) file,
about how to install and compile VTS-Registry.

#### Unpackaged deps

`cartolina-tileserver` is using (among other
libraries) [OpenMesh](https://www.openmesh.org/). You have to download and
install OpenMesh library and this is, how you do it

```
git clone https://www.graphics.rwth-aachen.de:9000/OpenMesh/OpenMesh.git
cd OpenMesh
mkdir build
cd build
cmake ..
make -j4
sudo make install
```

#### Installing packaged dependencies

Now we can download and install rest of the dependencies, which are needed to
get `cartolina-tileserver` compiled:

```
sudo apt-get install \
    libboost-dev \
    libboost-thread-dev \
    libboost-program-options-dev \
    libboost-filesystem-dev \
    libboost-regex-dev \
    libboost-iostreams-dev\
    libboost-python-dev \
    libopencv-dev libopencv-core-dev libopencv-highgui-dev \
    libopencv-photo-dev libopencv-imgproc-dev libeigen3-dev libgdal-dev \
    libproj-dev libgeographic-dev libjsoncpp-dev \
    libprotobuf-dev protobuf-compiler libprocps-dev libmagic-dev \
    libtinyxml2-dev libmarkdown2-dev \
    gawk sqlite3
```

### Clone and Download

The source code can be downloaded from this [repository](https://github.com/cartolinadev/cartolina-tileserver), 
but since there are external dependences, you have to use `--recursive` switch 
while cloning the repo.


```
git clone --recursive https://github.com/cartolinadev/cartolina-tileserver/cartolina-tileserver.git 
```

**NOTE:** If you did clone from GitHub previously without the `--recursive`
parameter, you should probably delete the `vts-mapproxy` directory and clone
again. The build will not work otherwise.

### Configure and build

For building the tileserver, you just have to use ``make``

```
cd cartolina-tileserver/mapproxy
make -j16 # to compile in 16 threads
```

Default target location (for later `make install`) is `/usr/local/` directory.
You can set the `CMAKE_INSTALL_PREFIX` variable, to change it:

```
make set-variable VARIABLE=CMAKE_INSTALL_PREFIX=/install/prefix
```

You should see compilation progress. Depending on how many threads you allowed for
the compilation (the `-jNUMBER` parameter) it might take a couple of minutes to an
hour of compilation time.

The binaries are then stored in `bin` directory. Development libraries are
stored in `lib` directory.

### Installing

You should be able to call `make install`, which will install to either default
location `/usr/local/` or to directory defined previously by the
`CMAKE_INSTALL_PREFIX` variable (see previous part).

When you specify the `DESTDIR` variable, resulting files will be saved in
`$DESTDIR/$CMAKE_INSTALL_PREFIX` directory (this is useful for packaging), e.g.

```
make install DESTDIR=/home/user/tmp/
```

### Configuring and running your own build 


First you need to create a `mapproxy.conf` configuration file. You can then run

```
mapproxy --help
mapproxy --config mapproxy.conf
```

**NOTE:** You might need to add also `--registry` parameter, and point it to
previously compiled [`vts-registry`](https://github.com/melowntech/vts-registry).

You will probably also need to create an nginx reverse proxy. This is exactly
what the `vts-backend` metapackage takes care of, if you choose to go without 
it, you need to do these steps manually. 


## How to contribute

Check the [CONTRIBUTING.md](CONTRIBUTING.md) file.


## License

`cartolina-tileserver` is open source under a permissive BSD 2-clause license. See
[LICENSE](LICENSE) for details.


