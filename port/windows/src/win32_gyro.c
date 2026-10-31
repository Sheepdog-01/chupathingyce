/*
WIN32_GYRO.C

The gyroscope for the touch controls' aim on Windows
(port/linux/src/touch_overlay.c), read at its fastest. SDL reads it too
(src/sensor/windows/SDL_windowssensor.c), but sets no report interval, so
Windows reports it at its default: a tenth of a second, on a Surface Pro 9,
too slow to aim by. Here the Sensor API's gyrometer is opened with the
shortest report interval it has (SENSOR_PROPERTY_MIN_REPORT_INTERVAL), and
its readings are kept for touch_overlay.c to take.

The event sink follows SDL's, the readings come on the thread that opened
the sensor (the game's, as its window's messages are pumped), and each is
timed on SDL_GetTicksNS's clock, as SDL's events are.
*/

#define COBJMACROS
#include <windows.h>
#include <sensorsapi.h>
#include <sensors.h>
#include <portabledevicetypes.h>

#include <SDL3/SDL.h>

void platform_log(const char *format, ...);

/* (the Sensor API's GUIDs and keys, as SDL keeps its own: the import
libraries that define them are not linked) */
static const CLSID gyro_clsid_sensor_manager =
	{ 0x77a1c827, 0xfcd2, 0x4689, { 0x89, 0x15, 0x9d, 0x61, 0x3c, 0xc5, 0xfa, 0x3e } };
static const IID gyro_iid_sensor_manager =
	{ 0xbd77db67, 0x45a8, 0x42dc, { 0x8d, 0x00, 0x6d, 0xcf, 0x15, 0xf8, 0x37, 0x7a } };
static const IID gyro_iid_sensor_events =
	{ 0x5d8dcc91, 0x4641, 0x47e7, { 0xb7, 0xc3, 0xb7, 0x4f, 0x48, 0xa6, 0xc3, 0x91 } };
static const IID gyro_iid_unknown =
	{ 0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const CLSID gyro_clsid_portable_device_values =
	{ 0x0c15d503, 0xd017, 0x47ce, { 0x90, 0x16, 0x7b, 0x3f, 0x97, 0x87, 0x21, 0xcc } };
static const IID gyro_iid_portable_device_values =
	{ 0x6848f6f2, 0x3155, 0x4f86, { 0xb6, 0xf5, 0x26, 0x3e, 0xee, 0xab, 0x31, 0x43 } };
static const GUID gyro_type_gyrometer_3d =
	{ 0x09485f5a, 0x759e, 0x42c2, { 0xbd, 0x4b, 0xa3, 0x49, 0xb7, 0x5c, 0x86, 0x43 } };
static const PROPERTYKEY gyro_min_report_interval =
	{ { 0x7f8383ec, 0xd3ec, 0x495c, { 0xa8, 0xcf, 0xb8, 0xbb, 0xe8, 0x5c, 0x29, 0x20 } }, 12 };
static const PROPERTYKEY gyro_current_report_interval =
	{ { 0x7f8383ec, 0xd3ec, 0x495c, { 0xa8, 0xcf, 0xb8, 0xbb, 0xe8, 0x5c, 0x29, 0x20 } }, 13 };
static const PROPERTYKEY gyro_rate_keys[3] =
{
	{ { 0x3f8a69a2, 0x07c5, 0x4e48, { 0xa9, 0x65, 0xcd, 0x79, 0x7a, 0xab, 0x56, 0xd5 } }, 10 },
	{ { 0x3f8a69a2, 0x07c5, 0x4e48, { 0xa9, 0x65, 0xcd, 0x79, 0x7a, 0xab, 0x56, 0xd5 } }, 11 },
	{ { 0x3f8a69a2, 0x07c5, 0x4e48, { 0xa9, 0x65, 0xcd, 0x79, 0x7a, 0xab, 0x56, 0xd5 } }, 12 },
};

/* the shortest interval asked for (250 a second), and the one asked for
when the sensor does not say */
#define GYRO_FASTEST_MS 4
#define GYRO_UNKNOWN_MS 10
/* readings kept between two takes */
#define GYRO_QUEUE 64

static struct
{
	ISensorManager *manager;
	ISensor *sensor;
	BOOL uninitialize;
	CRITICAL_SECTION lock;
	BOOL lock_ready;
	/* the readings not taken yet: their times and rates (radians a second
	about x, y and z, SDL's axes) */
	int count;
	unsigned long long times[GYRO_QUEUE];
	float rates[GYRO_QUEUE][3];
} gyro;

static BOOL same_iid(REFIID a, const IID *b)
{
	return memcmp(a, b, sizeof(IID)) == 0;
}

static HRESULT STDMETHODCALLTYPE events_query_interface(ISensorEvents *This, REFIID riid, void **object)
{
	if (!object)
		return E_INVALIDARG;
	*object = NULL;
	if (same_iid(riid, &gyro_iid_unknown) || same_iid(riid, &gyro_iid_sensor_events))
	{
		*object = This;
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE events_add_ref(ISensorEvents *This)
{
	(void)This;
	return 1;
}

static ULONG STDMETHODCALLTYPE events_release(ISensorEvents *This)
{
	(void)This;
	return 1;
}

static HRESULT STDMETHODCALLTYPE events_state_changed(ISensorEvents *This, ISensor *sensor, SensorState state)
{
	(void)This;
	(void)sensor;
	(void)state;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE events_data_updated(ISensorEvents *This, ISensor *sensor, ISensorDataReport *report)
{
	float rates[3];
	int axis;

	(void)This;
	if (sensor != gyro.sensor || !report)
		return S_OK;
	for (axis = 0; axis < 3; axis++)
	{
		PROPVARIANT value;

		PropVariantInit(&value);
		if (FAILED(ISensorDataReport_GetSensorValue(report, &gyro_rate_keys[axis], &value)) || value.vt != VT_R8)
		{
			PropVariantClear(&value);
			return S_OK;
		}
		rates[axis] = (float)value.dblVal * (SDL_PI_F / 180.0f);
		PropVariantClear(&value);
	}
	EnterCriticalSection(&gyro.lock);
	/* (a full queue loses its oldest: the newest rate is the one that
	matters) */
	if (gyro.count == GYRO_QUEUE)
	{
		memmove(gyro.times, gyro.times + 1, sizeof(gyro.times[0]) * (GYRO_QUEUE - 1));
		memmove(gyro.rates, gyro.rates + 1, sizeof(gyro.rates[0]) * (GYRO_QUEUE - 1));
		gyro.count--;
	}
	gyro.times[gyro.count] = SDL_GetTicksNS();
	memcpy(gyro.rates[gyro.count], rates, sizeof(rates));
	gyro.count++;
	LeaveCriticalSection(&gyro.lock);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE events_event(ISensorEvents *This, ISensor *sensor, REFGUID event,
	IPortableDeviceValues *data)
{
	(void)This;
	(void)sensor;
	(void)event;
	(void)data;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE events_leave(ISensorEvents *This, REFSENSOR_ID id)
{
	(void)This;
	(void)id;
	return S_OK;
}

static ISensorEventsVtbl events_vtbl =
{
	events_query_interface,
	events_add_ref,
	events_release,
	events_state_changed,
	events_data_updated,
	events_event,
	events_leave,
};
static ISensorEvents events = { &events_vtbl };

/* the report interval the sensor has now, in milliseconds, or 0 */
static ULONG report_interval(ISensor *sensor, const PROPERTYKEY *key)
{
	PROPVARIANT value;
	ULONG milliseconds = 0;

	PropVariantInit(&value);
	if (SUCCEEDED(ISensor_GetProperty(sensor, key, &value)) && value.vt == VT_UI4)
		milliseconds = value.ulVal;
	PropVariantClear(&value);
	return milliseconds;
}

/* asks for the shortest report interval the sensor has */
static void report_interval_set(ISensor *sensor)
{
	IPortableDeviceValues *values = NULL, *results = NULL;
	ULONG fastest = report_interval(sensor, &gyro_min_report_interval);
	ULONG asked = fastest ? (fastest < GYRO_FASTEST_MS ? GYRO_FASTEST_MS : fastest) : GYRO_UNKNOWN_MS;

	if (FAILED(CoCreateInstance(&gyro_clsid_portable_device_values, NULL, CLSCTX_INPROC_SERVER,
		&gyro_iid_portable_device_values, (void **)&values)))
	{
		return;
	}
	if (SUCCEEDED(IPortableDeviceValues_SetUnsignedIntegerValue(values, &gyro_current_report_interval, asked)))
		ISensor_SetProperties(sensor, values, &results);
	if (results)
		IPortableDeviceValues_Release(results);
	IPortableDeviceValues_Release(values);
	platform_log("touch: gyroscope reports every %lu ms (asked for %lu; the sensor's shortest is %lu)",
		report_interval(sensor, &gyro_current_report_interval), asked, fastest);
}

void windows_gyro_close(void)
{
	if (gyro.sensor)
	{
		ISensor_SetEventSink(gyro.sensor, NULL);
		ISensor_Release(gyro.sensor);
		gyro.sensor = NULL;
	}
	if (gyro.manager)
	{
		ISensorManager_Release(gyro.manager);
		gyro.manager = NULL;
	}
	if (gyro.uninitialize)
	{
		CoUninitialize();
		gyro.uninitialize = FALSE;
	}
	if (gyro.lock_ready)
	{
		EnterCriticalSection(&gyro.lock);
		gyro.count = 0;
		LeaveCriticalSection(&gyro.lock);
	}
}

/* opens the gyrometer at its fastest; nonzero if it is open */
int windows_gyro_open(void)
{
	ISensorCollection *sensors = NULL;
	ULONG count = 0;
	HRESULT result;

	if (gyro.sensor)
		return 1;
	if (!gyro.lock_ready)
	{
		InitializeCriticalSection(&gyro.lock);
		gyro.lock_ready = TRUE;
	}
	/* (as SDL's: an apartment; one this thread already has in another mode
	is used as it is) */
	result = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	gyro.uninitialize = SUCCEEDED(result);
	if (FAILED(result) && result != RPC_E_CHANGED_MODE)
		return 0;
	if (FAILED(CoCreateInstance(&gyro_clsid_sensor_manager, NULL, CLSCTX_INPROC_SERVER, &gyro_iid_sensor_manager,
		(void **)&gyro.manager)) ||
		FAILED(ISensorManager_GetSensorsByType(gyro.manager, &gyro_type_gyrometer_3d, &sensors)) ||
		FAILED(ISensorCollection_GetCount(sensors, &count)) || !count ||
		FAILED(ISensorCollection_GetAt(sensors, 0, &gyro.sensor)))
	{
		if (sensors)
			ISensorCollection_Release(sensors);
		gyro.sensor = NULL;
		windows_gyro_close();
		return 0;
	}
	ISensorCollection_Release(sensors);
	report_interval_set(gyro.sensor);
	if (FAILED(ISensor_SetEventSink(gyro.sensor, &events)))
	{
		windows_gyro_close();
		return 0;
	}
	return 1;
}

/* the readings since the last take, at most maximum: their times
(SDL_GetTicksNS's) and rates (3 each, radians a second); how many */
int windows_gyro_take(unsigned long long *times, float *rates, int maximum)
{
	int count;

	if (!gyro.lock_ready)
		return 0;
	EnterCriticalSection(&gyro.lock);
	count = gyro.count < maximum ? gyro.count : maximum;
	memcpy(times, gyro.times, sizeof(gyro.times[0]) * (size_t)count);
	memcpy(rates, gyro.rates, sizeof(gyro.rates[0]) * (size_t)count);
	memmove(gyro.times, gyro.times + count, sizeof(gyro.times[0]) * (size_t)(gyro.count - count));
	memmove(gyro.rates, gyro.rates + count, sizeof(gyro.rates[0]) * (size_t)(gyro.count - count));
	gyro.count -= count;
	LeaveCriticalSection(&gyro.lock);
	return count;
}
