
// LedDevice includes
#include <leddevice/LedDevice.h>
#include "ProviderRs232.h"
#include <utils/WaitTime.h>

// qt includes
#include <QEventLoop>
#include <QDir>

#include <chrono>

// Constants
namespace {
	const bool verbose = false;

	constexpr std::chrono::milliseconds WRITE_TIMEOUT{ 1000 };	// device write timeout in ms
	constexpr std::chrono::milliseconds OPEN_TIMEOUT{ 5000 };		// device open timeout in ms
	const int MAX_WRITE_TIMEOUTS = 5;	// Maximum number of allowed timeouts
	const int NUM_POWEROFF_WRITE_BLACK = 2;	// Number of write "BLACK" during powering off

	constexpr std::chrono::milliseconds DEFAULT_IDENTIFY_TIME{ 500 };

	// tty discovery service
	const char DISCOVERY_DIRECTORY[] = "/dev/";
	const char DISCOVERY_FILEPATTERN[] = "tty*";
} //End of constants

ProviderRs232::ProviderRs232(const QJsonObject &deviceConfig)
	: LedDevice(deviceConfig)
	  ,_rs232Port()
	  ,_baudRate_Hz(1000000)
	  ,_isAutoDeviceName(false)
	  ,_delayAfterConnect_ms(0)
	  ,_frameDropCounter(0)
{
}

bool ProviderRs232::init(const QJsonObject &deviceConfig)
{
	bool isInitOK = false;

	// Initialise sub-class
	if ( LedDevice::init(deviceConfig) )
	{

		Debug(_log, "DeviceType   : %s", QSTRING_CSTR( this->getActiveDeviceType() ));
		Debug(_log, "LedCount     : %d", this->getLedCount());
		Debug(_log, "ColorOrder   : %s", QSTRING_CSTR( this->getColorOrder() ));
		Debug(_log, "RefreshTime  : %d", _refreshTimerInterval_ms);
		Debug(_log, "LatchTime    : %d", this->getLatchTime());

		_deviceName           = deviceConfig["output"].toString("auto");
		_isAutoDeviceName     = _deviceName.toLower() == "auto";

		// If device name was given as unix /dev/ system-location, get port name
		if ( _deviceName.startsWith(QLatin1String("/dev/")) )
		{
			_location = _deviceName;
			//Handle udev devices
			QFileInfo file_info(_deviceName);
			if (file_info.isSymLink())
			{
				_deviceName = file_info.symLinkTarget();
			}
			_deviceName = _deviceName.mid(5);
		}

		_baudRate_Hz          = deviceConfig["rate"].toInt();
		_delayAfterConnect_ms = deviceConfig["delayAfterConnect"].toInt(1500);

		Debug(_log, "DeviceName   : %s", QSTRING_CSTR(_deviceName));
		DebugIf(!_location.isEmpty(), _log, "Location     : %s", QSTRING_CSTR(_location));
		Debug(_log, "AutoDevice   : %d", _isAutoDeviceName);
		Debug(_log, "baudRate_Hz  : %d", _baudRate_Hz);
		Debug(_log, "delayAfCon ms: %d", _delayAfterConnect_ms);

		isInitOK = true;
	}
	return isInitOK;
}

ProviderRs232::~ProviderRs232()
{
}

int ProviderRs232::open()
{
	int retval = -1;
	_isDeviceReady = false;
	_isInSwitchOff = false;

	// open device physically
	if ( tryOpen(_delayAfterConnect_ms) )
	{
		// Everything is OK, device is ready
		_isDeviceReady = true;
		retval = 0;
	}
	return retval;
}

int ProviderRs232::close()
{
	int retval = 0;

	_isDeviceReady = false;

	// Test, if device requires closing
	if (_rs232Port.isOpen())
	{
		_rs232Port.flush();
		Debug(_log,"Close UART: %s", QSTRING_CSTR(_deviceName) );
		_rs232Port.close();
		// Everything is OK -> device is closed
	}
	return retval;
}

bool ProviderRs232::powerOff()
{
	// Simulate power-off by writing a final "Black" to have a defined outcome
	bool rc = false;
	if ( writeBlack( NUM_POWEROFF_WRITE_BLACK ) >= 0 )
	{
		rc = true;
	}
	return rc;
}

bool ProviderRs232::tryOpen(int delayAfterConnect_ms)
{
	if (_deviceName.isEmpty())
	{
		if (!_rs232Port.isOpen())
		{
			if ( _isAutoDeviceName )
			{
				_deviceName = discoverFirst();
				if (_deviceName.isEmpty())
				{
					this->setInError( QString("No serial device found automatically!") );
					return false;
				}
			}
		}
	}

	if (!_rs232Port.isOpen() && !_deviceName.isEmpty())
	{
		if (!_location.isEmpty())
		{
			Info(_log, "Opening UART: %s (%s)", QSTRING_CSTR(_deviceName), QSTRING_CSTR(_location));
		}
		else
		{
			Info(_log, "Opening UART: %s", QSTRING_CSTR(_deviceName));
		}

		_frameDropCounter = 0;

		_rs232Port.setPort(_location.toStdString());
		_rs232Port.setBaudrate( _baudRate_Hz );

		Debug(_log, "_rs232Port.open(QIODevice::ReadWrite): %s, Baud rate [%d]bps", QSTRING_CSTR(_deviceName), _baudRate_Hz);


		try {
			_rs232Port.open();
		}
		catch (std::invalid_argument) {
			Error(_log, "_rs232Port.open() failed: Invalid parameter!");
		}
	}

	if (delayAfterConnect_ms > 0)
	{

		Debug(_log, "delayAfterConnect for %d ms - start", delayAfterConnect_ms);

		// Wait delayAfterConnect_ms before allowing write
		QEventLoop loop;
		QTimer::singleShot(delayAfterConnect_ms, &loop, &QEventLoop::quit);
		loop.exec();

		Debug(_log, "delayAfterConnect for %d ms - finished", delayAfterConnect_ms);
	}

	return _rs232Port.isOpen();
}

void ProviderRs232::setInError(const QString& errorMsg)
{
	// _rs232Port.clearError();
	this->close();

	LedDevice::setInError( errorMsg );
}

int ProviderRs232::writeBytes(const qint64 size, const uint8_t *data)
{
	int rc = 0;
	if (!_rs232Port.isOpen())
	{
		Debug(_log, "!_rs232Port.isOpen()");

		if ( !tryOpen(OPEN_TIMEOUT.count()) )
		{
			return -1;
		}
	}
	size_t bytesWritten = _rs232Port.write(data, (size_t)size);
	if (bytesWritten == -1 || bytesWritten != size)
	{
		this->setInError( QString ("Rs232 SerialPortError: %1").arg("No error") );
		rc = -1;
	}
	/*
	else
	{
		_rs232Port.waitByteTimes(WRITE_TIMEOUT.count());
		if (!_rs232Port.waitReadable())
		{
				this->setInError( QString ("Rs232 SerialPortError: %1").arg("No errro") );
				rc = -1;
		}
	}
	*/
	return rc;
}

QString ProviderRs232::discoverFirst()
{
	// take first available USB serial port - currently no probing!
	for (auto & portInfo : serial::list_ports())
	{
		if (!portInfo.port.empty())
		{
			auto portName = QString::fromStdString(portInfo.port);
			Info(_log, "found serial device: %s", portName);
			return QString::fromStdString(portInfo.port);
		}
	}
	return "";
}

QJsonObject ProviderRs232::discover(const QJsonObject& /*params*/)
{
	QJsonObject devicesDiscovered;
	devicesDiscovered.insert("ledDeviceType", _activeDeviceType );

	QJsonArray deviceList;

	// Discover serial Devices
	for (auto &portInfo : serial::list_ports() )
	{
		if ( !portInfo.port.empty() && !portInfo.hardware_id.empty() )
		{
			QJsonObject portInfoJson;
			portInfoJson.insert("description", QString::fromStdString(portInfo.description));
			portInfoJson.insert("manufacturer", QString::fromStdString(portInfo.hardware_id));
			portInfoJson.insert("portName", QString::fromStdString(portInfo.port));
			deviceList.append(portInfoJson);
		}
	}

#ifndef _WIN32
	//Check all /dev/tty* files, if they are udev-serial devices
	QDir deviceDirectory (DISCOVERY_DIRECTORY);
	QStringList deviceFilter(DISCOVERY_FILEPATTERN);
	deviceDirectory.setNameFilters(deviceFilter);
	deviceDirectory.setSorting(QDir::Name);
	QFileInfoList deviceFiles = deviceDirectory.entryInfoList(QDir::AllEntries);

	QFileInfoList::const_iterator deviceFileIterator;
	for (deviceFileIterator = deviceFiles.constBegin(); deviceFileIterator != deviceFiles.constEnd(); ++deviceFileIterator)
	{
		if ((*deviceFileIterator).isSymLink())
		{
			auto port = serial::Serial((*deviceFileIterator).symLinkTarget().toStdString());
			QJsonObject portInfoJson;
			portInfoJson.insert("portName", (*deviceFileIterator).fileName());
			portInfoJson.insert("systemLocation", (*deviceFileIterator).absoluteFilePath());
			portInfoJson.insert("udev", true);

			portInfoJson.insert("portName", QString::fromStdString(port.getPort()));

			deviceList.append(portInfoJson);
		}
	}
#endif

	devicesDiscovered.insert("devices", deviceList);
	DebugIf(verbose,_log, "devicesDiscovered: [%s]", QString(QJsonDocument(devicesDiscovered).toJson(QJsonDocument::Compact)).toUtf8().constData());

	return devicesDiscovered;
}

void ProviderRs232::identify(const QJsonObject& params)
{
	DebugIf(verbose,_log, "params: [%s]", QString(QJsonDocument(params).toJson(QJsonDocument::Compact)).toUtf8().constData());

	QString deviceName = params["output"].toString("");
	if (!deviceName.isEmpty())
	{
		_devConfig = params;
		init(_devConfig);
		{
			if ( open() == 0 )
			{
				for (int i = 0; i < 2; ++i)
				{
					if (writeColor(ColorRgb::RED) == 0)
					{
						wait(DEFAULT_IDENTIFY_TIME);

						writeColor(ColorRgb::BLACK);
						wait(DEFAULT_IDENTIFY_TIME);
					}
					else
					{
						break;
					}
				}
				close();
			}
		}
	}
}
