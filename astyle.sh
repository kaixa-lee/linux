dir=`pwd`
filelist=`find $dir -type f -name "*.c"`
 
for file in $filelist
do
    astyle --style=linux --indent=spaces=4 --add-braces $file
done    
