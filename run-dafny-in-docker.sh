docker build -t ironsync-artifact .

docker run --rm -v $PWD/build:/root/ironsync/build ironsync-artifact ./run-verifier.sh
