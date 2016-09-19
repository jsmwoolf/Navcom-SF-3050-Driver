#include "navcom3050lib.h"

//////////////////////////////////////////////
// Class specific declarations
//////////////////////////////////////////////

navcom3050::navcom3050(int mySize)
{
	bufSize = mySize;
	myBuf = new unsigned char[bufSize];
}

navcom3050::~navcom3050()
{
	if (fd != 0)
		closeDevice();
	delete [] myBuf;
}

//////////////////////////////////////////////
// Functions for system calls
//////////////////////////////////////////////

/*
Function: openDevice
Purpose: Opens the Navcom SF 3050
*/
void navcom3050::openDevice()
{
	fd = open("/dev/navcom-SF-3050-",O_RDWR);
}

/*
Function: closeDevice
Purpose: Closes the Navcom SF 3050
*/
int navcom3050::closeDevice()
{
	return close(fd);
}

/*
Function: writeToDevice
Purpose: Writes data to the Navcom SF 3050
*/
ssize_t navcom3050::writeToDevice(char *buf)
{
	if (buf[0] != '[')
		return -1;
	return write(fd,buf,strlen(buf));
}

/*
Function: readFromDevice
Purpose: Reads data from the Navcom SF 3050
*/
ssize_t navcom3050::readFromDevice()
{
	unsigned char temp[64];
	int pos = 0;
	ssize_t r = 2;
	while(temp[r-1] != '\n' && temp[r-2] != '\r')
	{
		r = read(fd,temp,64);
		if (r < 0)
			break;
		std::cout << r << std::endl;
		for(int x = 0; x < r; ++x)
		{
			myBuf[pos++] = temp[x];
		}
		std::cout << (unsigned int)temp[r-2] << " " << (unsigned int)temp[r-1] << std::endl;
	}

	if (checkIntegrety(r))
		return pos;
	return pos;
}

//////////////////////////////////////////////
// Functions for other functionality
//////////////////////////////////////////////

/*
Function: isOpen
Purpose: Checks whether the Navcom SF 3050 is currently open
*/
bool navcom3050::isOpen()
{
	return (fd > 0);
}

/*
Function: getBuffer
Purpose: Return the buffer that we retrieved from the GPS
*/
unsigned char * navcom3050::getBuffer()
{
	return myBuf;
}

/*
Function: getMnemonic
Purpose: Return the mnemonic that was retrived from the buffer
*/
std::string navcom3050::getMnemonic()
{
	std::string result = "";
	int pos = 0;	
	char delim;
	if (myBuf[0] == '[')
		delim = ']';

	do
	{
		result += myBuf[pos];
	} while(myBuf[pos++] != delim);

	return result;
}

/*
Function: getLatitude
Purpose: Return the latitude of the GPS
*/
float navcom3050::getLatitude()
{
	int lat = 0;
	if (getMnemonic() != "[PVT1B]")
		return -1;
	for(int x=0;x<4;++x)
		lat += (unsigned int)myBuf[17+x] << 8*x;
	return (float)((lat / 2048) + ( ((myBuf[25] & 0xF0) >> 8) /32768))/3600;
}

/*
Function: getLongitude
Purpose: Return the longitude of the GPS
*/
float navcom3050::getLongitude()
{
	int lon = 0;
	// Works only with PVT1B command
	if (getMnemonic() != "[PVT1B]")
		return -1;
	for(int x=0;x<4;++x)
		lon += (unsigned int)myBuf[21+x] << 8*x;
	return (float)((lon / 2048) + ( ((myBuf[25] & 0x0F)) /32768))/3600;
}

/*
Function: getVelocities
Purpose: Return three velocities in the following order:
	    1) North
	    2) East
	    3) Up
*/
double * navcom3050::getVelocities()
{
	double *vel = new double[3];
	if (getMnemonic() != "[PVT1B]")
		return vel;
	int temp;
	for(int x = 0; x< 3;++x)
	{
		temp = 0;
		for(int y =0; y<3; ++y)
			temp += (unsigned int)myBuf[(42 + 3*x)+y] << 8*y ;
		vel[x] = ( ((temp & 0x800000) != 0 ? 0xFF000000 : 0x0) | temp) / 1024.;
	}
	return vel;
}
/*
Function: crc_CCITT
Purpose: Performs a 16-bit cyclic redundancy check.
NOTE: This should never be called by the user!
*/
unsigned short int crc_CCITT(unsigned char *buf, short int length)
{
	unsigned short int accum;
	for (accum = 0; length!= 0; length--, buf++)
		accum = (unsigned short int)((accum << 8) ^ CrcTable[(accum >> 8) ^ *buf]);
	return ( accum );
}

unsigned int crc_ASCII(unsigned short int crcword)
{
	unsigned int accum=0;
	unsigned char lsbyte;
	
	unsigned char icount;
	
	for (icount = 0; icount<4; icount++)
	{
		lsbyte = (unsigned char) (crcword & 0x0000000F);
		//printf("lsbyte before is %x \n", lsbyte);
		if  ( (lsbyte>-1) && (lsbyte<10) )
		{	
			lsbyte = lsbyte + 48;  // convert 0 to 9 to ascii
		}
		else if ( (lsbyte>9) && (lsbyte<16) )
		{
			lsbyte = lsbyte + 55;  // convert A to F to ascii
		}
		else
		{
			lsbyte = 0;
		}
		//printf("lsbyte after is %x \n", lsbyte);
		accum = accum + (unsigned int) (lsbyte << (8*icount));
		crcword = crcword >>4;
	}
	
	return ( accum );
}

/*
Function: checkIntegrety
Purpose: Check whether the recieved message is valid.
NOTE:  This function currently does not work.
*/
bool navcom3050::checkIntegrety(ssize_t len)
{
	unsigned short int checkSum = crc_CCITT(myBuf,(unsigned short)len);
	unsigned int checkSum32 = crc_ASCII(checkSum);

	std::cout << checkSum32 << " " << checkSum << std::endl;
	return false;
}
