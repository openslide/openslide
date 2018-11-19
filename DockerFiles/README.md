# Compiling OpenSlide

This directory contains a docker image and docker-compose configuration to build openslide for windows on linux using 
docker and for building a linux debian package.


```bash
# Start the docker container
docker-compose run openslide

# In the container run one of the following scripts:
bash build_win.sh 
bash build_linux.sh
bash build_linux_deb.sh

# Alternatively, run the build directly:
docker-compose run openslide bash build_win.sh
docker-compose run openslide bash build_linux.sh
docker-compose run openslide bash build_linux_deb.sh

```