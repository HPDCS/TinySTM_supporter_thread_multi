
cd tinySTM
echo ./make_all.sh
./make_all.sh
cd ..

cd stamp
cd yada
echo make -f Makefile.stm clean
make -f Makefile.stm clean
echo make -f Makefile.stm 
make -f Makefile.stm 
cd ..
cd ..

