// STL includes
#include <algorithm>
#include <limits>

// qt incl
#include <QDateTime>
#include <QTimer>

// Hyperion includes
#include <hyperion/PriorityMuxer.h>

// utils
#include <utils/Logger.h>

const int PriorityMuxer::FG_PRIORITY = 1;
const int PriorityMuxer::BG_PRIORITY = 254;
const int PriorityMuxer::MANUAL_SELECTED_PRIORITY = 256;
const int PriorityMuxer::LOWEST_PRIORITY = std::numeric_limits<uint8_t>::max();
const int PriorityMuxer::TIMEOUT_NOT_ACTIVE_PRIO = -100;

PriorityMuxer::PriorityMuxer(int ledCount, QObject * parent)
	: QObject(parent)
	, _log(nullptr)
	, _currentPriority(PriorityMuxer::LOWEST_PRIORITY)
	, _previousPriority(_currentPriority)
	, _manualSelectedPriority(MANUAL_SELECTED_PRIORITY)
	, _prevVisComp (hyperion::Components::COMP_COLOR)
	, _activeInputs()
	, _lowestPriorityInfo()
	, _sourceAutoSelectEnabled(true)
	, _updateTimer(new QTimer(this))
	, _timer(new QTimer(this))
	, _blockTimer(new QTimer(this))
{
	QString subComponent = parent->property("instance").toString();
	_log= Logger::getInstance("MUXER", subComponent);

	// init lowest priority info
	_lowestPriorityInfo.priority       = PriorityMuxer::LOWEST_PRIORITY;
	_lowestPriorityInfo.timeoutTime_ms = -1;
	_lowestPriorityInfo.ledColors      = std::vector<ColorRgb>(ledCount, {0, 0, 0});
	_lowestPriorityInfo.componentId    = hyperion::COMP_COLOR;
	_lowestPriorityInfo.origin         = "System";
	_lowestPriorityInfo.owner          = "";

	_activeInputs[PriorityMuxer::LOWEST_PRIORITY] = _lowestPriorityInfo;

	// adapt to 1s interval for COLOR and EFFECT timeouts > -1
	connect(_timer, &QTimer::timeout, this, &PriorityMuxer::timeTrigger);
	_timer->setSingleShot(true);
	_blockTimer->setSingleShot(true);
	// forward timeRunner signal to prioritiesChanged signal & threading workaround
	connect(this, &PriorityMuxer::timeRunner, this, &PriorityMuxer::prioritiesChanged);
	connect(this, &PriorityMuxer::signalTimeTrigger, this, &PriorityMuxer::timeTrigger);

	// start muxer timer
	connect(_updateTimer, &QTimer::timeout, this, &PriorityMuxer::setCurrentTime);
	_updateTimer->setInterval(250);
	_updateTimer->start();
}

PriorityMuxer::~PriorityMuxer()
{
}

void PriorityMuxer::setEnable(bool enable)
{
	enable ? _updateTimer->start() : _updateTimer->stop();
}

bool PriorityMuxer::setSourceAutoSelectEnabled(bool enable, bool update)
{
	if(_sourceAutoSelectEnabled != enable)
	{
		// on disable we need to make sure the last priority call to setPriority is still valid
		if(!enable && !_activeInputs.contains(_manualSelectedPriority))
		{
			Warning(_log, "Can't disable auto selection, as the last manual selected priority (%d) is no longer available", _manualSelectedPriority);
			return false;
		}

		_sourceAutoSelectEnabled = enable;
		Debug(_log, "Source auto select is now %s", enable ? "enabled" : "disabled");

		// update _currentPriority if called from external
		if(update)
			setCurrentTime();

		return true;
	}
	return false;
}

bool PriorityMuxer::setPriority(int priority)
{
	if(_activeInputs.contains(priority))
	{
		_manualSelectedPriority = priority;
		// update auto select state -> update _currentPriority
		setSourceAutoSelectEnabled(false);
		return true;
	}
	return false;
}

void PriorityMuxer::updateLedColorsLength(int ledCount)
{
	for (auto infoIt = _activeInputs.begin(); infoIt != _activeInputs.end();)
	{
		if (!infoIt->ledColors.empty())
		{
			infoIt->ledColors.resize(ledCount, infoIt->ledColors.at(0));
		}
		++infoIt;
	}
}

QList<int> PriorityMuxer::getPriorities() const
{
	return _activeInputs.keys();
}

bool PriorityMuxer::hasPriority(int priority) const
{
	return (priority == PriorityMuxer::LOWEST_PRIORITY) ? true : _activeInputs.contains(priority);
}

PriorityMuxer::InputInfo PriorityMuxer::getInputInfo(int priority) const
{
	auto elemIt = _activeInputs.find(priority);
	if (elemIt == _activeInputs.end())
	{
		elemIt = _activeInputs.find(PriorityMuxer::LOWEST_PRIORITY);
		if (elemIt == _activeInputs.end())
		{
			// fallback
			return _lowestPriorityInfo;
		}
	}
	return elemIt.value();
}

hyperion::Components PriorityMuxer::getComponentOfPriority(int priority) const
{
	return _activeInputs[priority].componentId;
}

void PriorityMuxer::registerInput(int priority, hyperion::Components component, const QString& origin, const QString& owner, unsigned smooth_cfg)
{
	// detect new registers
	bool newInput = false;
	bool reusedInput = false;
	if (!_activeInputs.contains(priority))
		newInput = true;
	else if(_prevVisComp == component || _activeInputs[priority].componentId == component)
		reusedInput = true;

	InputInfo& input     = _activeInputs[priority];
	input.priority       = priority;
	input.timeoutTime_ms = newInput ? TIMEOUT_NOT_ACTIVE_PRIO : input.timeoutTime_ms;
	input.componentId    = component;
	input.origin         = origin;
	input.smooth_cfg     = smooth_cfg;
	input.owner          = owner;

	if (newInput)
	{
		Debug(_log,"Register new input '%s/%s' with priority %d as inactive", QSTRING_CSTR(origin), hyperion::componentToIdString(component), priority);
		// emit 'prioritiesChanged' only if _sourceAutoSelectEnabled is false
		if (!_sourceAutoSelectEnabled)
		{
			emit prioritiesChanged();
		}
		return;
	}

	if (reusedInput)
	{
		emit timeRunner();
	}
}

bool PriorityMuxer::setInput(int priority, const std::vector<ColorRgb>& ledColors, int64_t timeout_ms)
{
	if(!_activeInputs.contains(priority))
	{
		Error(_log,"setInput() used without registerInput() for priority '%d', probably the priority reached timeout",priority);
		return false;
	}

	InputInfo& input = _activeInputs[priority];
	// detect active <-> inactive changes
	bool activeChange = false;
	bool active = true;

	// calculate final timeout
	if (timeout_ms >= 0)
	{
		timeout_ms = QDateTime::currentMSecsSinceEpoch() + timeout_ms;
	}
	else if (input.timeoutTime_ms >= 0)
	{
		timeout_ms = QDateTime::currentMSecsSinceEpoch();
	}

	if(input.timeoutTime_ms == TIMEOUT_NOT_ACTIVE_PRIO && timeout_ms != TIMEOUT_NOT_ACTIVE_PRIO)
	{
		activeChange = true;
	}
	else if(timeout_ms == TIMEOUT_NOT_ACTIVE_PRIO && input.timeoutTime_ms != TIMEOUT_NOT_ACTIVE_PRIO)
	{
		active = false;
		activeChange = true;
	}
	// update input
	input.timeoutTime_ms = timeout_ms;
	input.ledColors      = ledColors;
	input.image.clear();

	// emit active change
	if(activeChange)
	{
		Debug(_log, "Priority %d is now %s", priority, active ? "active" : "inactive");
		if (_currentPriority < priority)
		{
			emit prioritiesChanged();
		}
		setCurrentTime();
	}

	return true;
}

bool PriorityMuxer::setInputImage(int priority, const Image<ColorRgb>& image, int64_t timeout_ms)
{
	if(!_activeInputs.contains(priority))
	{
		Error(_log,"setInputImage() used without registerInput() for priority '%d', probably the priority reached timeout",priority);
		return false;
	}

	InputInfo& input = _activeInputs[priority];
	// detect active <-> inactive changes
	bool activeChange = false;
	bool active = true;

	// calculate final timeout
	if (timeout_ms >= 0)
	{
		timeout_ms = QDateTime::currentMSecsSinceEpoch() + timeout_ms;
	}
	else if (input.timeoutTime_ms >= 0)
	{
		timeout_ms = QDateTime::currentMSecsSinceEpoch();
	}

	if(input.timeoutTime_ms == TIMEOUT_NOT_ACTIVE_PRIO && timeout_ms != TIMEOUT_NOT_ACTIVE_PRIO)
	{
		activeChange = true;
	}
	else if(timeout_ms == TIMEOUT_NOT_ACTIVE_PRIO && input.timeoutTime_ms != TIMEOUT_NOT_ACTIVE_PRIO)
	{
		active = false;
		activeChange = true;
	}
	// update input
	input.timeoutTime_ms = timeout_ms;
	input.image          = image;
	input.ledColors.clear();

	// emit active change
	if(activeChange)
	{
		Debug(_log, "Priority %d is now %s", priority, active ? "active" : "inactive");
		if (_currentPriority < priority)
		{
			emit prioritiesChanged();
		}
		setCurrentTime();
	}

	return true;
}

bool PriorityMuxer::setInputInactive(int priority)
{
	Image<ColorRgb> image;
	return setInputImage(priority, image, TIMEOUT_NOT_ACTIVE_PRIO);
}

bool PriorityMuxer::clearInput(int priority)
{
	if (priority < PriorityMuxer::LOWEST_PRIORITY && (_activeInputs.remove(priority) > 0))
	{
		Debug(_log,"Removed source priority %d",priority);
		// on clear success update _currentPriority
		setCurrentTime();
		// emit 'prioritiesChanged' only if _sourceAutoSelectEnabled is false
		if ((!_sourceAutoSelectEnabled && (_currentPriority < priority)) || _currentPriority == BG_PRIORITY)
			emit prioritiesChanged();
		return true;
	}
	return false;
}

void PriorityMuxer::clearAll(bool forceClearAll)
{
	if (forceClearAll)
	{
		_previousPriority = _currentPriority;
		_activeInputs.clear();
		_currentPriority = PriorityMuxer::LOWEST_PRIORITY;
		_activeInputs[_currentPriority] = _lowestPriorityInfo;
	}
	else
	{
		for(auto key : _activeInputs.keys())
		{
			const InputInfo info = getInputInfo(key);
			if ((info.componentId == hyperion::COMP_COLOR || info.componentId == hyperion::COMP_EFFECT || info.componentId == hyperion::COMP_IMAGE) && key < PriorityMuxer::LOWEST_PRIORITY-1)
			{
				clearInput(key);
			}
		}
	}
}

void PriorityMuxer::setCurrentTime()
{
	const int64_t now = QDateTime::currentMSecsSinceEpoch();
	int newPriority;
	_activeInputs.contains(0) ? newPriority = 0 : newPriority = PriorityMuxer::LOWEST_PRIORITY;

	for (auto infoIt = _activeInputs.begin(); infoIt != _activeInputs.end();)
	{
		if (infoIt->timeoutTime_ms > 0 && infoIt->timeoutTime_ms <= now)
		{
			int tPrio = infoIt->priority;
			infoIt = _activeInputs.erase(infoIt);
			Debug(_log,"Timeout clear for priority %d",tPrio);
			emit prioritiesChanged();
		}
		else
		{
			// timeoutTime of TIMEOUT_NOT_ACTIVE_PRIO is awaiting data (inactive); skip
			if(infoIt->timeoutTime_ms > TIMEOUT_NOT_ACTIVE_PRIO)
				newPriority = qMin(newPriority, infoIt->priority);

			// call timeTrigger when effect or color is running with timeout > 0, blacklist prio 255
			if (infoIt->priority < BG_PRIORITY && infoIt->timeoutTime_ms > 0 && (infoIt->componentId == hyperion::COMP_EFFECT || infoIt->componentId == hyperion::COMP_COLOR || infoIt->componentId == hyperion::COMP_IMAGE))
			{
				emit signalTimeTrigger(); // as signal to prevent Threading issues
			}
			++infoIt;
		}
	}
	// evaluate, if manual selected priority is still available
	if(!_sourceAutoSelectEnabled)
	{
		if(_activeInputs.contains(_manualSelectedPriority))
		{
			newPriority = _manualSelectedPriority;
		}
		else
		{
			Debug(_log, "The manual selected priority '%d' is no longer available, switching to auto selection", _manualSelectedPriority);
			// update state, but no _currentPriority re-eval
			setSourceAutoSelectEnabled(true, false);
		}
	}
	// apply & emit on change (after apply!)
	hyperion::Components comp = getComponentOfPriority(newPriority);
	if (_currentPriority != newPriority || comp != _prevVisComp)
	{
		_previousPriority = _currentPriority;
		_currentPriority = newPriority;
		Debug(_log, "Set visible priority to %d", newPriority);
		emit visiblePriorityChanged(newPriority);
		// check for visible comp change
		if (comp != _prevVisComp)
		{
			_prevVisComp = comp;
			emit visibleComponentChanged(comp);
		}
		emit prioritiesChanged();
	}
}

void PriorityMuxer::timeTrigger()
{
	if(_blockTimer->isActive())
	{
		_timer->start(500);
	}
	else
	{
		emit timeRunner();
		_blockTimer->start(1000);
	}
}
