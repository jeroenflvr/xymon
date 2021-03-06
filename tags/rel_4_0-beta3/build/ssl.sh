	echo "Checking for OpenSSL ..."

	OSSLINC=""
	OSSLLIB=""
	for DIR in /opt/openssl* /opt/ssl* /usr/local/openssl* /usr/local/ssl* /usr/local /usr
	do
		if test -d $DIR/include/openssl
		then
			OSSLINC=$DIR/include
		fi

		if test -f $DIR/lib/libcrypto.so
		then
			OSSLLIB=$DIR/lib
		fi
		if test -f $DIR/lib/libcrypto.a
		then
			OSSLLIB=$DIR/lib
		fi
	done

	if test -z "$OSSLINC" -o -z "$OSSLLIB"; then
		echo "OpenSSL include- or library-files not found."
		echo "Although you can use bbgen/bbgend without OpenSSL, you will not"
		echo "be able to run network tests of SSL-enabled services, e.g. https."
		echo "So installing OpenSSL is recommended."
		echo "OpenSSL can be found at http://www.openssl.org/"
		echo "Continuing with SSL support disabled."
	else

		cd build
		OS=`uname -s` make -f Makefile.test-ssl clean
		OS=`uname -s` OSSLINC="-I$OSSLINC" make -f Makefile.test-ssl test-compile
		if [ $? -eq 0 ]; then
			echo "Found OpenSSL include files in $OSSLINC"
		else
			echo "WARNING: OpenSSL include files found in $OSSLINC, but compile fails."
		fi
	
		OS=`uname -s` OSSLLIB="-L$OSSLLIB" make -f Makefile.test-ssl test-link
		if [ $? -eq 0 ]; then
			echo "Found OpenSSL libraries in $OSSLLIB"
		else
			echo "WARNING: OpenSSL library files found in $OSSLLIB, but link fails."
		fi
		OS=`uname -s` make -f Makefile.test-ssl clean
		cd ..

	fi

