FROM --platform=linux/amd64 ubuntu:18.04

# https://askubuntu.com/questions/909277/avoiding-user-interaction-with-tzdata-when-installing-certbot-in-a-docker-contai
ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update
RUN apt-get install -y git make wget unzip
RUN apt-get install -y vim emacs
RUN apt-get install -y python3-pip time
RUN apt-get install -y sloccount graphviz

RUN pip3 install toposort

# dependencies for compilation 
# RUN apt-get install -y clang
# RUN apt-get install -y libc++-dev
# RUN apt-get install -y libc++abi-dev
# RUN apt-get install -y libdb5.3-stl-dev
# RUN apt-get install -y libdb-dev libdb++-dev
# RUN apt-get install -y texlive texlive-pictures

# install dafny 3.0 dependencies 
RUN wget https://packages.microsoft.com/config/ubuntu/18.04/packages-microsoft-prod.deb -O packages-microsoft-prod.deb
RUN dpkg -i packages-microsoft-prod.deb
RUN rm packages-microsoft-prod.deb
RUN apt-get update;
RUN apt-get install -y apt-transport-https
RUN apt-get update
RUN apt-get install -y dotnet-sdk-5.0

WORKDIR /root
RUN mkdir ironsync

COPY linear-dafny /root/ironsync/linear-dafny
WORKDIR /root/ironsync

COPY tools        /root/ironsync/tools
RUN tools/install-dafny.sh

COPY Makefile	    /root/ironsync/Makefile
COPY build-tests	/root/ironsync/build-tests
COPY lib          /root/ironsync/lib
COPY concurrency	/root/ironsync/concurrency

COPY run-verifier.sh /root/ironsync/
COPY build-cache-source.sh /root/ironsync/
