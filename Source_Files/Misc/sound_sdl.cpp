/*
 *  sound_sdl.cpp - Sound management, SDL specific stuff
 *
 *  Written in 2000 by Christian Bauer
 */


// Number of sound channels used by sound manager
const int SM_SOUND_CHANNELS = MAXIMUM_SOUND_CHANNELS + MAXIMUM_AMBIENT_SOUND_CHANNELS;

// Private channels
const int MUSIC_CHANNEL = SM_SOUND_CHANNELS;
const int RESOURCE_CHANNEL = SM_SOUND_CHANNELS + 1;

// Total number of sound channels
const int TOTAL_SOUND_CHANNELS = SM_SOUND_CHANNELS + 2;


// SDL sound channel data
struct sdl_channel {
	bool active;			// Flag: currently playing sound

	bool sixteen_bit;		// Flag: 16-bit sound data (8-bit otherwise)
	bool stereo;			// Flag: stereo sound data (mono otherwise)
	int bytes_per_frame;	// Bytes per sample frame (1, 2 or 4)

	uint8 *data;			// Current pointer to sound data
	int32 length;			// Length in bytes remaining to be played
	uint8 *loop;			// Pointer to loop start
	int32 loop_length;		// Loop length in bytes (0 = don't loop)

	fixed rate;				// Sample rate (relative to output sample rate)
	fixed counter;			// Counter for up/downsampling

	int16 left_volume;		// Volume (0x100 = nominal)
	int16 right_volume;

	uint8 *next_header;		// Pointer to next queued sound header (NULL = none)
	fixed next_pitch;		// Pitch of next queued sound header
} sdl_channels[TOTAL_SOUND_CHANNELS];


// Sound buffer sizes
const int MINIMUM_SOUND_BUFFER_SIZE = 300*KILO;
const int MORE_SOUND_BUFFER_SIZE = 600*KILO;
const int AMBIENT_SOUND_BUFFER_SIZE = 1*MEG;
const int MAXIMUM_SOUND_BUFFER_SIZE = 1*MEG;


// Desired and obtained audio formats
static SDL_AudioSpec desired;

// Global sound volume (0x100 = nominal)
static int16 main_volume = 0x100;


// From FileHandler_SDL.cpp
extern void get_default_sounds_spec(FileSpecifier &file);

// From shell_sdl.cpp
extern bool option_nosound;

// Prototypes
static void close_sound_file(void);
static void shutdown_sound_manager(void);
static void set_sound_manager_status(bool active);
static void load_sound_header(sdl_channel *c, uint8 *data, fixed pitch);
static void sound_callback(void *userdata, uint8 *stream, int len);


/*
 *  Open sounds file, parse sound definitions
 */

static _bs_field _bs_sound_file_header[] = { // 260 bytes
	_4byte, _4byte, _2byte, _2byte, 124*sizeof(int16)
};

bool open_sound_file(FileSpecifier &File)
{
	if (!(sound_definitions && _sm_globals))
		return false;

	// Open file
	if (!File.Open(SoundFile))
		return false;

	// Read and check header
	struct sound_file_header header;
	if (!SoundFile.ReadObject(header)) {
		SoundFile.Close();
		return false;
	}

	byte_swap_data(&header, 260, 1, _bs_sound_file_header);

	if (header.version != SOUND_FILE_VERSION ||
		header.tag != SOUND_FILE_TAG ||
		header.sound_count != NUMBER_OF_SOUND_DEFINITIONS ||
		header.source_count != NUMBER_OF_SOUND_SOURCES) {
		SoundFile.Close();
		return false;
	}

	// Read sound definitions
	if (!SoundFile.ReadObjectList(NUMBER_OF_SOUND_SOURCES * NUMBER_OF_SOUND_DEFINITIONS, sound_definitions)) {
		SoundFile.Close();
		return false;
	}

	byte_swap_data(sound_definitions, 64, NUMBER_OF_SOUND_SOURCES * NUMBER_OF_SOUND_DEFINITIONS, _bs_sound_definition);
	return true;
}


/*
 *  Close sounds file
 */

static void close_sound_file(void)
{
	SoundFile.Close();
}


/*
 *  Initialize sound manager
 */

static void initialize_machine_sound_manager(struct sound_manager_parameters *parameters)
{
	OSErr error;

	// Assign channels
	for (int i=0; i<SM_SOUND_CHANNELS; i++)
		_sm_globals->channels[i].channel = sdl_channels + i;

	// Initialize _sm_globals
	_sm_globals->loaded_sounds_size = 0;
	_sm_globals->total_channel_count = 0;

	// Open sounds file
	FileSpecifier InitialSoundFile;
	get_default_sounds_spec(InitialSoundFile);
	if (open_sound_file(InitialSoundFile)) {
		atexit(shutdown_sound_manager);

		_sm_globals->available_flags = _stereo_flag | _dynamic_tracking_flag
		                             | _ambient_sound_flag | _more_sounds_flag
		                             | _16bit_sound_flag | _extra_memory_flag;

		// Set parameters and start sound manager
		_sm_parameters->flags = 0;
		_sm_initialized = true;
		_sm_active = false;
		set_sound_manager_parameters(parameters);
		set_sound_manager_status(true);
	}
}


/*
 *  Shutdown sound manager
 */

static void shutdown_sound_manager(void)
{
	set_sound_manager_status(false);
	close_sound_file();
}


/*
 *  Enable/disable sound manager
 */

static void set_sound_manager_status(bool active)
{
	if (_sm_initialized) {
		if (active != _sm_active) {
			if (active) {

				// Set channel count
				_sm_globals->total_channel_count = _sm_parameters->channel_count;
				if (_sm_parameters->flags & _ambient_sound_flag)
					_sm_globals->total_channel_count += MAXIMUM_AMBIENT_SOUND_CHANNELS;

				// Set buffer size
				_sm_globals->total_buffer_size = (_sm_parameters->flags & _more_sounds_flag) ? MORE_SOUND_BUFFER_SIZE : MINIMUM_SOUND_BUFFER_SIZE;
				if (_sm_parameters->flags & _ambient_sound_flag)
					_sm_globals->total_buffer_size += AMBIENT_SOUND_BUFFER_SIZE;
				if (_sm_parameters->flags & _16bit_sound_flag)
					_sm_globals->total_buffer_size *= 2;
				if (_sm_globals->available_flags & _extra_memory_flag)
					_sm_globals->total_buffer_size *= 2;

				_sm_globals->sound_source = (_sm_parameters->flags & _16bit_sound_flag) ? _16bit_22k_source : _8bit_22k_source;
				_sm_globals->base_sound_definitions = sound_definitions + _sm_globals->sound_source * NUMBER_OF_SOUND_DEFINITIONS;

				// Initialize channels
				struct channel_data *channel = _sm_globals->channels;
				for (int i=0; i<_sm_globals->total_channel_count; i++, channel++) {
					channel->flags = 0;
					channel->callback_count = 0;
					channel->sound_index = NONE;
					sdl_channel *c = (sdl_channel *)channel->channel;
					memset(c, 0, sizeof(sdl_channel));
					c->left_volume = c->right_volume = 0x100;
				}

				// Set global volume
				main_volume = _sm_parameters->volume * SOUND_VOLUME_DELTA;

				// Activate SDL audio
				desired.freq = (_sm_parameters->pitch >> 16) * 22050;
				desired.format = _sm_parameters->flags & _16bit_sound_flag ? AUDIO_S16SYS : AUDIO_S8;
				desired.channels = _sm_parameters->flags & _stereo_flag ? 2 : 1;
				desired.samples = 1024;
				desired.callback = sound_callback;
				desired.userdata = NULL;
				if (option_nosound || SDL_OpenAudio(&desired, NULL) < 0) {
					if (!option_nosound)
						alert_user(infoError, strERRORS, badSoundChannels, -1);
					_sm_globals->total_channel_count = 0;
					active = _sm_active = _sm_initialized = false;
				}
printf("freq %d, channels %d\n", desired.freq, desired.channels);
				SDL_PauseAudio(false);

			} else {

				// Stop sound manager
				stop_all_sounds();
				SDL_CloseAudio();
				_sm_globals->total_channel_count = 0;
			}
			_sm_active = active;
		}
	}
}


/*
 *  Set sound manager preferences
 */

void set_sound_manager_parameters(struct sound_manager_parameters *parameters)
{
	if (_sm_initialized) {
		bool initial_state = _sm_active;

		verify_sound_manager_parameters(parameters);

		// If it was initially on, turn off the sound manager
		if (initial_state)
			set_sound_manager_status(false);

		// We need to get rid of the sounds we have in memory
		unload_all_sounds();

		// Stuff in our new parameters
		*_sm_parameters = *parameters;

		// If it was initially on, turn the sound manager back on
		if (initial_state)
			set_sound_manager_status(true);
	}
}


/*
 *  Adjust sound volume
 */

boolean adjust_sound_volume_up(struct sound_manager_parameters *parameters, short sound_index)
{
	if (_sm_active && parameters->volume < NUMBER_OF_SOUND_VOLUME_LEVELS) {
		_sm_parameters->volume = (parameters->volume += 1);
		main_volume = parameters->volume * SOUND_VOLUME_DELTA;
		play_sound(sound_index, (world_location3d *)NULL, NONE);
		return true;
	}
	return false;
}

boolean adjust_sound_volume_down(struct sound_manager_parameters *parameters, short sound_index)
{
	if (_sm_active && parameters->volume > 0) {
		_sm_parameters->volume = (parameters->volume -= 1);
		main_volume = parameters->volume * SOUND_VOLUME_DELTA;
		play_sound(sound_index, (world_location3d *)NULL, NONE);
		return true;
	}
	return false;
}


/*
 *  Is channel busy?
 */

static boolean channel_busy(struct channel_data *channel)
{
	assert(SLOT_IS_USED(channel));
	sdl_channel *c = (sdl_channel *)channel->channel;
	return c->active;
}


/*
 *  Unlock sound
 */

static void unlock_sound(short sound_index)
{
	// nothing to do
}


/*
 *  Dispose of sound
 */

static void dispose_sound(short sound_index)
{
	struct sound_definition *definition = get_sound_definition(sound_index);
	if (!definition)
		return;
	
	if (!definition->ptr)
		return;

	_sm_globals->loaded_sounds_size -= definition->size;
	free(definition->ptr);
	definition->ptr = NULL;
	definition->size = 0;
}


/*
 *  Read sound from file, return pointer to data (should be asynchronous and only read
 *  a single sound unless _sm_parameters->flags & _more_sounds_flag)
 */

static byte *read_sound_from_file(short sound_index, int &size)
{
	size = 0;

	struct sound_definition *definition = get_sound_definition(sound_index);
	if (!definition)
		return NULL;

	uint8 *data = NULL;
	OSErr error= noErr;
	
	if (SoundFile.IsOpen()) {
		size = (_sm_parameters->flags&_more_sounds_flag) ? definition->total_length : definition->single_length;

		if ((data = (uint8 *)malloc(size)) != NULL) {
			if (!SoundFile.SetPosition(definition->group_offset)) {
				free(data);
				data = NULL;
				error = SoundFile.GetError();
			}
			if (SoundFile.Read(size, data))
				_sm_globals->loaded_sounds_size += size;
			else {
				free(data);
				data = NULL;
				error = SoundFile.GetError();
			}
		} else
			error = -1;
	}
	
	vwarn(error==noErr, csprintf(temporary, "read_sound_from_file(#%d) got error #%d", sound_index, error));
	
	return data;
}


/*
 *  Stop sounds on channel
 */

static void quiet_channel(struct channel_data *channel)
{
	sdl_channel *c = (sdl_channel *)channel->channel;
	c->active = false;
}


/*
 *  Set variables for sound channel
 */

static void instantiate_sound_variables(struct sound_variables *variables, struct channel_data *channel, boolean first_time)
{
	if (first_time || variables->right_volume != channel->variables.right_volume || variables->left_volume != channel->variables.left_volume) {
		sdl_channel *c = (sdl_channel *)channel->channel;
		c->left_volume = variables->left_volume;
		c->right_volume = variables->right_volume;
	}
	channel->variables = *variables;
}


/*
 *  Start sound playback (asynchronously)
 */

static void buffer_sound(struct channel_data *channel, short sound_index, fixed pitch)
{
	struct sound_definition *definition = get_sound_definition(sound_index);
	if (!definition)
		return;
	assert(definition->ptr);

	int permutation = get_random_sound_permutation(sound_index);
	assert(permutation>=0 && permutation<definition->permutations);

	// Lock sound subsystem
	sdl_channel *c = (sdl_channel *)channel->channel;
	SDL_LockAudio();

	// Get pointer to sound header
	uint8 *data = (uint8 *)definition->ptr + definition->sound_offsets[permutation];

	// Channel active? Then queue next header
	if (c->active) {

		// Yes, queue mext header
		c->next_header = data;
		c->next_pitch = pitch;

	} else {

		// No, load sound header
		load_sound_header(c, data, pitch);
		c->active = true;
	}

	// Unlock sound subsystem
	SDL_UnlockAudio();
}


/*
 *  Start playback of MacOS sound resource
 */

void play_sound_resource(void *sound, uint32 sound_size)
{
printf("*** play_sound_resource %p, size %d\n", sound, sound_size);
	//!!
}

void stop_sound_resource(void)
{
	sdl_channels[RESOURCE_CHANNEL].active = false;
}


/*
 *  Sound callback function
 */

static void load_sound_header(sdl_channel *c, uint8 *data, fixed pitch)
{
	SDL_RWops *p = SDL_RWFromMem(data, 64);
	if (p == NULL) {
		c->active = false;
		return;
	}

	// Get sound header type, skip unused sample pointer
	uint8 header_type = data[20];
	SDL_RWseek(p, 4, SEEK_CUR);

	// Parse sound header
	c->bytes_per_frame = 1;
	if (header_type == 0x00) {			// Standard sound header
		c->data = data + 22;
		c->sixteen_bit = c->stereo = false;
		c->length = SDL_ReadBE32(p);
		c->rate = (pitch >> 8) * ((SDL_ReadBE32(p) >> 8) / desired.freq);
		uint32 loop_start = SDL_ReadBE32(p);
		c->loop = c->data + loop_start;
		c->loop_length = SDL_ReadBE32(p) - loop_start;
	} else if (header_type == 0xff) {	// Extended sound header
		c->data = data + 64;
		c->stereo = SDL_ReadBE32(p) == 2;
		if (c->stereo)
			c->bytes_per_frame *= 2;
		c->rate = (pitch >> 8) * ((SDL_ReadBE32(p) >> 8) / desired.freq);
		uint32 loop_start = SDL_ReadBE32(p);
		c->loop = c->data + loop_start;
		c->loop_length = SDL_ReadBE32(p) - loop_start;
		SDL_RWseek(p, 2, SEEK_CUR);
		c->length = SDL_ReadBE32(p) * c->bytes_per_frame;
		SDL_RWseek(p, 22, SEEK_CUR);
		c->sixteen_bit = SDL_ReadBE16(p) == 16;
		if (c->sixteen_bit)
			c->bytes_per_frame *= 2;
		c->length *= c->bytes_per_frame;
	} else {							// Unknown header type
		fprintf(stderr, "Unknown sound header type %02x\n", header_type);
		c->active = false;
		SDL_FreeRW(p);
		return;
	}

	// Correct loop count
	if (c->loop_length < 4)
		c->loop_length = 0;

	// Reset sample counter
	c->counter = 0;

	SDL_FreeRW(p);
}

template <class T>
inline static void calc_buffer(T *p, int len, bool stereo)
{
	while (len--) {
		int32 left = 0, right = 0;	// 16-bit internally

		// Mix all channels
		sdl_channel *c = sdl_channels;
		for (int i=0; i<_sm_globals->total_channel_count; i++, c++) {

			// Channel active?
			if (c->active) {

				// Yes, read sound data
				int32 dleft, dright;
				if (c->stereo) {
					if (c->sixteen_bit) {
						dleft = (int16)SDL_SwapBE16(0[(int16 *)c->data]);
						dright = (int16)SDL_SwapBE16(1[(int16 *)c->data]);
					} else {
						dleft = (int32)(int8)(0[c->data] ^ 0x80) << 8;
						dright = (int32)(int8)(1[c->data] ^ 0x80) << 8;
					}
				} else {
					if (c->sixteen_bit)
						dleft = dright = (int16)SDL_SwapBE16(*(int16 *)c->data);
					else
						dleft = dright = (int32)(int8)(*(c->data) ^ 0x80) << 8;
				}

				// Mix into output
				left += (dleft * c->left_volume) >> 8;
				right += (dright * c->right_volume) >> 8;

				// Advance sound data pointer
				c->counter += c->rate;
				if (c->counter >= 0x10000) {
					int count = c->counter >> 16;
					c->counter &= 0xffff;
					c->data += c->bytes_per_frame * count;
					c->length -= c->bytes_per_frame * count;

					// Sound finished? Then enter loop or load next sound header
					if (c->length <= 0) {

						// Yes, loop present?
						if (c->loop_length) {

							// Yes, enter loop
							c->data = c->loop;
							c->length = c->loop_length;

						} else {

							// No, another sound header queued?
							_sm_globals->channels[i].callback_count++;
							if (c->next_header) {

								// Yes, load sound header and continue
								load_sound_header(c, c->next_header, c->next_pitch);
								c->next_header = NULL;

							} else {

								// No, turn off channel
								c->active = false;
							}
						}
					}
				}
			}
		}

		// Apply main volume setting
		left = (left * main_volume) >> 8;
		right = (right * main_volume) >> 8;

		// Clip output values
		if (left > 32767)
			left = 32767;
		else if (left < -32768)
			left = -32768;
		if (stereo) {
			if (right > 32767)
				right = 32767;
			else if (right < -32768)
				right = -32768;
		}

		// Downscale for 8-bit output
		if (sizeof(T) == 1) {
			left >>= 8;
			right >>= 8;
		}

		// Write to output buffer
		*p++ = left;
		if (stereo)
			*p++ = right;
	}
}

static void sound_callback(void *usr, uint8 *stream, int len)
{
	bool stereo = (desired.channels == 2);
	if ((desired.format & 0xff) == 16) {
		if (stereo)	// try to inline as much as possible
			calc_buffer((int16 *)stream, len / 4, true);
		else
			calc_buffer((int16 *)stream, len / 2, false);
	} else {
		if (stereo)
			calc_buffer((int8 *)stream, len / 2, true);
		else
			calc_buffer((int8 *)stream, len, false);
	}
}
