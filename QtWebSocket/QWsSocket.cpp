#include "QWsSocket.h"

int QWsSocket::maxBytesPerFrame = 1400;

QWsSocket::QWsSocket(QObject * parent)
	: QTcpSocket(parent)
{
	setSocketState( QAbstractSocket::UnconnectedState );

	connect( this, SIGNAL(readyRead()), this, SLOT(dataReceived()) );
	connect( this, SIGNAL(aboutToClose()), this, SLOT(aboutToClose()) );
}

QWsSocket::~QWsSocket()
{
}

void QWsSocket::dataReceived()
{
	QByteArray BA; // ReadBuffer
	quint8 byte; // currentByteBuffer

	// FIN, RSV1-3, Opcode
	BA = QIODevice::read(1);
	byte = BA[0];
	quint8 FIN = (byte >> 7);
	quint8 RSV1 = ((byte & 0x7F) >> 6);
	quint8 RSV2 = ((byte & 0x3F) >> 5);
	quint8 RSV3 = ((byte & 0x1F) >> 4);
	EOpcode Opcode = (EOpcode)(byte & 0x0F);

	// Mask, PayloadLength
	BA = QIODevice::read(1);
	byte = BA[0];
	quint8 Mask = (byte >> 7);
	quint64 PayloadLength = (byte & 0x7F);
	// Extended PayloadLength
	if ( PayloadLength == 126 )
	{
		BA = QIODevice::read(2);
		PayloadLength = (BA[0] << 1*8) + (BA[1] << 0*8);
	}
	else if ( PayloadLength == 127 )
	{
		BA = QIODevice::read(8);
		PayloadLength = ((quint64)BA[0] << 7*8) + ((quint64)BA[1] << 6*8) + ((quint64)BA[2] << 5*8) + ((quint64)BA[3] << 4*8) + ((quint64)BA[4] << 3*8) + ((quint64)BA[5] << 2*8) + ((quint64)BA[6] << 1*8) + ((quint64)BA[7] << 0*8);
	}

	// MaskingKey
	QByteArray MaskingKey;
	if ( Mask )
	{
		MaskingKey = QIODevice::read(4);
	}

	// ExtensionData
	QByteArray ExtensionData;
	// Extension // UNSUPPORTED FOR NOW

	// ApplicationData
	if ( PayloadLength )
	{
		QByteArray ApplicationData = QIODevice::read( PayloadLength );
		if ( Mask )
			ApplicationData = QWsSocket::mask( ApplicationData, MaskingKey );
		currentFrame.append( ApplicationData );
	}

	if ( Opcode == OpPong )
	{
		quint64 ms = pingTimer.elapsed();
		emit pong(ms);
	}
	else if ( Opcode == OpPing )
	{
		QByteArray pongRequest = QWsSocket::composeHeader( true, OpPong, 0 );
		write( pongRequest );
	}
	else if ( FIN )
	{
		if ( Opcode == OpBinary )
			emit frameReceived( currentFrame );
		else if ( Opcode == OpText )
			emit frameReceived( QString(currentFrame) );
		currentFrame.clear();
	}

	if ( bytesAvailable() )
		dataReceived();
}

QByteArray QWsSocket::readFrame()
{
	QByteArray frameToReturn = currentFrame;
	currentFrame.clear();
	return frameToReturn;
}

qint64 QWsSocket::write ( const QString & string, int maxFrameBytes )
{
	if ( maxFrameBytes == 0 )
		maxFrameBytes = maxBytesPerFrame;

	QList<QByteArray> framesList = QWsSocket::composeFrames( string.toUtf8(), false, maxFrameBytes );
	return writeFrames( framesList );
}

qint64 QWsSocket::write ( const QByteArray & byteArray, int maxFrameBytes )
{
	if ( maxFrameBytes == 0 )
		maxFrameBytes = maxBytesPerFrame;

	QList<QByteArray> framesList = QWsSocket::composeFrames( byteArray, true, maxFrameBytes );
	return writeFrames( framesList );
}

qint64 QWsSocket::writeFrame ( const QByteArray & byteArray )
{
	return QIODevice::write( byteArray );
}

qint64 QWsSocket::writeFrames ( QList<QByteArray> framesList )
{
	qint64 nbBytesWritten = 0;
	for ( int i=0 ; i<framesList.size() ; i++ )
	{
		nbBytesWritten += writeFrame( framesList[i] );
	}
	return nbBytesWritten;
}

void QWsSocket::close( QString reason )
{
	// Compose and send close frame
	quint64 messageSize = reason.size();
	QByteArray maskingKey = generateMaskingKey();
	QByteArray BA;
	quint8 byte;

	QByteArray header = QWsSocket::composeHeader( true, OpClose, 0 );
	BA.append( header );

	// Reason // UNSUPPORTED FOR NOW
	
	QAbstractSocket::write( BA );

	QAbstractSocket::close();
}

void QWsSocket::aboutToClose()
{
	close( "Connection closed by QWsSocket" );
}

QByteArray QWsSocket::generateMaskingKey()
{
	QByteArray key;
	for ( int i=0 ; i<4 ; i++ )
	{
		key.append( qrand() % 0x100 );
	}

	return key;
}

QByteArray QWsSocket::mask( QByteArray data, QByteArray maskingKey )
{
	for ( int i=0 ; i<data.size() ; i++ )
	{
		data[i] = ( data[i] ^ maskingKey[ i % 4 ] );
	}

	return data;
}

QList<QByteArray> QWsSocket::composeFrames( QByteArray byteArray, bool asBinary, int maxFrameBytes )
{
	if ( maxFrameBytes == 0 )
		maxFrameBytes = maxBytesPerFrame;

	QList<QByteArray> framesList;

	QByteArray maskingKey = generateMaskingKey();

	int nbFrames = byteArray.size() / maxFrameBytes + 1;

	for ( int i=0 ; i<nbFrames ; i++ )
	{
		QByteArray BA;

		// fin, size
		bool fin = false;
		quint64 size = maxFrameBytes;
		EOpcode opcode = OpContinue;
		if ( i == nbFrames-1 ) // for multi-frames
		{
			fin = true;
			size = byteArray.size();
		}
		if ( i == 0 )
		{
			if ( asBinary )
				opcode = OpBinary;
			else
				opcode = OpText;
		}
		
		// Header
		QByteArray header = QWsSocket::composeHeader( fin, opcode, size, maskingKey );
		BA.append( header );
		
		// Application Data
		QByteArray dataForThisFrame = byteArray.left( size );
		byteArray.remove( 0, size );
		
		dataForThisFrame = QWsSocket::mask( dataForThisFrame, maskingKey );
		BA.append( dataForThisFrame );
		
		framesList << BA;
	}

	return framesList;
}

QByteArray QWsSocket::composeHeader( bool fin, EOpcode opcode, quint64 payloadLength, QByteArray maskingKey )
{
	QByteArray BA;
	quint8 byte;

	// FIN, RSV1-3, Opcode
	byte = 0x00;
	// FIN
	if ( fin )
		byte = (byte | 0x80);
	// Opcode
	byte = (byte | opcode);
	BA.append( byte );

	// Mask, PayloadLength
	byte = 0x00;
	QByteArray BAsize;
	// Mask
	if ( maskingKey.size() == 4 )
		byte = (byte | 0x80);
	// PayloadLength
	if ( payloadLength <= 125 )
	{
		byte = (byte | payloadLength);
	}
	// Extended payloadLength
	else
	{
		// 2 bytes
		if ( payloadLength <= 0xFFFF )
		{
			byte = ( byte | 126 );
			BAsize.append( ( payloadLength >> 1*8 ) & 0xFF );
			BAsize.append( ( payloadLength >> 0*8 ) & 0xFF );
		}
		// 8 bytes
		else if ( payloadLength <= 0x7FFFFFFF )
		{
			byte = ( byte | 127 );
			BAsize.append( ( payloadLength >> 7*8 ) & 0xFF );
			BAsize.append( ( payloadLength >> 6*8 ) & 0xFF );
			BAsize.append( ( payloadLength >> 5*8 ) & 0xFF );
			BAsize.append( ( payloadLength >> 4*8 ) & 0xFF );
			BAsize.append( ( payloadLength >> 3*8 ) & 0xFF );
			BAsize.append( ( payloadLength >> 2*8 ) & 0xFF );
			BAsize.append( ( payloadLength >> 1*8 ) & 0xFF );
			BAsize.append( ( payloadLength >> 0*8 ) & 0xFF );
		}
	}
	BA.append( byte );
	BA.append( BAsize );

	// Masking
	if ( maskingKey.size() == 4 )
		BA.append( maskingKey );

	return BA;
}

void QWsSocket::ping()
{
	pingTimer.restart();
	QByteArray pingFrame = QWsSocket::composeHeader( true, OpPing, 0 );
	writeFrame( pingFrame );
}