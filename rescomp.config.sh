# note: also edit Makefile.am when compiling on rescomp

# tell configure which gcc to use:
export CXX=/apps/well/gcc/5.4.0/bin/gcc

# before execution:
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/apps/well/boost/1.59.0-gcc5.4.0-py27/lib
export PATH=/apps/well/python/2.7.10/bin:$PATH

# python packages
pip install --user sqlalchemy
