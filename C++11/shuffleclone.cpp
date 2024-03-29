// ------------------------------------------------------------
// Shuffleclone -- Shuffletron in C++11.
// Description: Minimum viable music player.
// Author: Andy Hefner <ahefner@gmail.com>
// License: MIT-style
// ------------------------------------------------------------

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cstring>
#include <cctype>
#include <cmath>

#include <algorithm>
#include <vector>
#include <deque>
#include <set>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <memory>
#include <exception>

#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <wordexp.h>
#include <arpa/inet.h>

#include <ao/ao.h>
#include <mpg123.h>
#include <pthread.h>

using std::cout;
using std::cin;
using std::endl;
using std::ifstream;
using std::ofstream;
using std::ios;
using std::printf;
using std::string;
using std::vector;
using std::deque;
using std::set;
using std::pair;
using std::unique_ptr;
using std::move;

// ------------------------------------------------------------
// Supporting definitions

// Simple Haskell-inspired option type.
template <typename T>
struct Maybe {
    bool present;
    T value;

    Maybe() : present(false) {}
    Maybe(T value) : present(true), value(value) {}

    void set (T const& t) { value = t; present = true; }
    void reset () { present = false; }

    Maybe<T>& operator= (T const& new_value) {
        value = new_value;
        present = true;
        return *this;
    }

    Maybe<T>& operator|= (T const& new_value) {
        value = new_value;
        present = true;
        return *this;
    }

    Maybe<T>& operator|= (Maybe<T> const& rhs) {
        if (!present && (this != &rhs)) *this = rhs;
        return *this;
    }

    // Defaulting value: (Maybe<T> | T) => T
    T operator|| (T alternative) const {
        return present? value : alternative;
    }

    // Combining: (Maybe<T> || Maybe<T>) => Maybe<T>
    Maybe<T> const& operator|| (Maybe<T> const& alternative) const {
        return present? *this : alternative;
    }
};

template <typename T>
Maybe<T> Just (T v) { return Maybe<T>(v); }

template <typename T>
Maybe<T> Nothing () { return Maybe<T>(); }

// ------------------------------------------------------------

// Normalize case and remove (most) punctuation, for search queries.
string munge (string const& s, int strip_tail=0)
{
    string munged;
    munged.reserve(s.length());
    int n = s.length() - strip_tail;
    if (n <= 0) return "";
    for (int in=0; in<n; in++) {
        char c = std::tolower(s[in]);
        if (std::isalnum(c) || c=='/') munged.push_back(c);
    }

    return munged;
}

// ------------------------------------------------------------
// Songs and streams

// Represents a song file on disk. Song objects live for the lifetime
// of the program.
struct Song
{
    string pathname;            // Full path to song, including filename.
    string filename;            // File name only, not including path.
    string munged;              // Munged pathname, for searching.

    Maybe<string> artist, album, title;
    Maybe<int> track;

    Song(string const& pathname, string const& filename)
        : pathname(pathname),
          filename(filename),
          munged(munge(pathname,4))
    { }
};

struct seek_command {
    enum { absolute, relative } mode;
    long offset;
};

// A song_stream is instantiated to play a particular song. Created by
// the spooler, consumed by the audio thread.
struct song_stream
{
    Song *song;
    mpg123_handle *mh;
    bool paused;
    bool eof;
    long long time_samples;
    long long length_samples;
    long rate;
    int channels;
    Maybe<seek_command> seek_to;

    song_stream(Song *song, mpg123_handle *mh)
        : song(song), mh(mh), paused(false), eof(false),
          time_samples(0),
          length_samples(mpg123_length(mh))
    {
        int encoding;
        mpg123_getformat(mh,&rate,&channels,&encoding);
    }

    size_t read (unsigned char *buffer, size_t size)
    {
        if (seek_to.present) {
            off_t sampleoff = seek_to.value.offset * rate;
            int whence = seek_to.value.mode == seek_command::absolute? SEEK_SET : SEEK_CUR;
            mpg123_seek(mh, sampleoff, whence);
            seek_to.reset();
            time_samples = mpg123_tell(mh);
        }

        if (eof) return 0;
        else if (paused) return size;
        else {
            const int bytes_per_sample = 4;
            size_t bytes_read;
            int code = mpg123_read(mh, buffer, size, &bytes_read);
            size_t samples_read = bytes_read / bytes_per_sample;
            switch (code) {
            case MPG123_DONE:
                eof = true;
                time_samples += samples_read;
                return bytes_read;

            case MPG123_OK:
                time_samples += samples_read;
                return bytes_read;

            default:
                //cout << mpg123_strerror(mh) << endl;
                eof = true;
                return 0;
            }
        }
    }

    ~song_stream() {
        mpg123_close(mh);
        mpg123_delete(mh);
    }
};

// ------------------------------------------------------------
// Preferences / Storage

string preferences_root ()
{
    const char *home = std::getenv("HOME");
    assert(home != NULL);
    string root = home + string("/.shuffleclone-c++");
    mkdir(root.c_str(), 0700);
    return root;
}

string prefpath (string pref)
{
    return preferences_root() + "/" + pref;
}

template <typename T>
Maybe<T> getpref (const char *name)
{
    ifstream in(prefpath(name).c_str(), ifstream::in);
    if (in.is_open()) {
        T tmp;
        in >> tmp;
        return Just(tmp);
    } else return Nothing<T>();
}

// ------------------------------------------------------------
// Playback Queue / Spooler
//
// The Spooler holds the (double-ended) queue of songs the user has
// arranged to play, and is responsible for readying an audio stream
// corresponding to the song at the head of the queue which can be
// immediately handed off upon request of the audio thread.

class Spooler
{
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;

    deque<Song *> song_queue;
    bool running;
    unique_ptr<song_stream> preloaded;

    // Disable copy/assignment.
    Spooler(Spooler const&);
    Spooler& operator= (Spooler const&);

public:
    Spooler() : thread(0), running(true)
    {
        pthread_mutex_init(&mutex, NULL);
        pthread_cond_init(&cond, NULL);
    }

    void lock () {
        assert(0==pthread_mutex_lock(&mutex));
    }

    void unlock () {
        // Maintain an invariant that if 'preloaded' is non-NULL, it
        // must refer to the song at the head of the queue. Since the
        // queue can only be modified while holding the mutex, this is
        // the ideal place to do it.
        if (preloaded.get()) {

            if (song_queue.empty() || (preloaded->song != song_queue.front())) {
                // cout << "flushing preloaded stream.\n";
                preloaded.reset();
            }
        }
        // Wake the spooler thread.
        pthread_cond_signal(&cond);
        assert(0==pthread_mutex_unlock(&mutex));
    }

    void assert_locked () {
        assert(pthread_mutex_trylock(&mutex) == EBUSY);
    }

    void start () {
        assert(0==pthread_create(&thread, NULL, thread_main, this));
    }

    void shutdown () {
        lock();
        running = false;
        unlock();
        pthread_join(thread,NULL);
        preloaded.reset();
    }

    void prequeue (vector<Song*> const& songs) {
        lock();
        for (auto i=songs.rbegin(); i!=songs.rend(); ++i) song_queue.push_front(*i);
        unlock();
    }

    void enqueue (vector<Song*> const& songs) {
        lock();
        for (auto song : songs) song_queue.push_back(song);
        unlock();
    }

    deque<Song*>& get_song_queue () {
        assert_locked();
        return song_queue;
    }

    void set_song_queue (deque<Song*> const& new_queue) {
        assert_locked();
        song_queue = new_queue;
    }

    // Polled by the audio thread to get the next song. Must not block.
    unique_ptr<song_stream> pop_next () {
        if (!pthread_mutex_trylock(&mutex)) {

            unique_ptr<song_stream> popped;
            popped = move(preloaded);

            if (popped.get()) {
                assert(popped->song == song_queue.front());
                song_queue.pop_front();
            }

            unlock();
            return popped;
        } else return unique_ptr<song_stream>();
    }

protected:
    void unlock_and_wait() {
        pthread_cond_wait(&cond, &mutex);
    }

    void run () {
        lock();
        while (running) {
            // Entering this point, the mutex is acquired, either
            // initially above, or by pthread_cond_wait during the
            // previous pass through this loop.

            // Preload the next song in the queue, so it's ready when
            // the audio thread needs it.
            if (!song_queue.empty()) {
                Song *song = song_queue[0];

                if (!preloaded.get() || (preloaded->song != song)) {
                    //cout << "spooling " << song->filename << endl;
                    mpg123_handle *mh = mpg123_new(NULL,NULL);
                    if (mh) {
                        long flags = MPG123_FORCE_STEREO | MPG123_QUIET;
                        mpg123_param(mh, MPG123_ADD_FLAGS, flags, 0.0);
                        mpg123_param(mh, MPG123_FORCE_RATE, 44100, 0.0);
                        if (MPG123_OK == mpg123_open(mh,song->pathname.c_str())) {
                            mpg123_scan(mh);
                            preloaded.reset(new song_stream(song,mh));
                        } else {
                            mpg123_delete(mh);
                            mh = NULL;
                        }
                    }

                    if (mh == NULL) {
                        // If there's an error opening this file, drop it
                        // from the playback queue, otherwise we're stuck
                        // waiting for it.
                        song_queue.pop_front();
                    }
                }
            }

            // After waiting, the mutex is again acquired.
            unlock_and_wait();
        }
        unlock();
    }

    static void *thread_main (void *arg) {
        static_cast<Spooler*>(arg)->run();
        return NULL;
    }

} spooler;

// ------------------------------------------------------------
// Audio thread

class AudioThread
{
    pthread_t thread;
    pthread_mutex_t mutex;
    bool requested_shutdown;
    ao_device *device;

    unique_ptr<song_stream> current_stream;

    // Disable copy/assignment.
    AudioThread(AudioThread const&);
    AudioThread& operator= (AudioThread const&);

public:
    AudioThread() : thread(0), requested_shutdown(false)
    {
        pthread_mutex_init(&mutex, NULL);
    }

    void lock () {
        assert(0==pthread_mutex_lock(&mutex));
    }

    void unlock () {
        assert(0==pthread_mutex_unlock(&mutex));
    }

    bool start () {
        char matrix[] = "L,R";  // I think libao forgot to const its char* here.
        ao_sample_format format = { 16, 44100, 2, AO_FMT_LITTLE, matrix };
        int driver_id = ao_default_driver_id();
        const char *device_name = ao_driver_info(driver_id)->name;

        device = ao_open_live(driver_id, &format, NULL);

        if (device) {
            //cout << "Using audio driver \"" << device_name << "\"\n";
            assert(0==pthread_create(&thread, NULL, thread_main, this));
            return true;
        } else {
            cout << "Failed to open audio device \"" << device_name << "\"\n";
            return false;
        }
    }

    void play (unique_ptr<song_stream> new_stream) {

        // Atomically swap the current stream with the new one..
        lock();
        auto old_stream = move(current_stream);
        current_stream = move(new_stream);
        unlock();

        // When we exit this scope, the old_stream gets freed by the
        // unique_ptr. I specifically wanted this to occur outside the
        // lock, so as to not block the audio thread, in case for some
        // reason it takes longer than it should.
    }

    void stop () {
        play(unique_ptr<song_stream>());
    }

    void seek (seek_command seek) {
        lock();
        if (current_stream.get()) current_stream->seek_to = seek;
        unlock();
    }

    void shutdown () {
        stop();
        assert(!current_stream.get());
        requested_shutdown = true;
        pthread_join(thread,NULL);
        ao_close(device);
    }

    void toggle_pause () {
        lock();
        if (current_stream.get()) {
            current_stream->paused = !current_stream->paused;
        }
        unlock();
    }

    struct state_description {
        Song *current_song;
        bool paused;
        long long time_samples;
        long long length_samples;
        double time;
        double length;
    };

    Maybe<state_description> current_state () {
        Maybe<state_description> sd;

        lock();
        if (current_stream.get()) {
            state_description d;
            d.current_song = current_stream->song;
            d.paused = current_stream->paused;
            d.time_samples = current_stream->time_samples;
            d.time = d.time_samples / (float)current_stream->rate;
            d.length_samples = current_stream->length_samples;
            d.length = d.length_samples / (float)current_stream->rate;
            sd.set(d);
        }
        unlock();

        return sd;
    }

protected:
    // Main loop for the audio thread. Runs until requested_shutdown
    // is set by the UI. Pops song_streams from the spooler, drives
    // the decoder, outputs the result.
    void run ()
    {
        unsigned char buffer[4096*2*2];

        while (!requested_shutdown)
        {
            size_t bytes_available = sizeof(buffer);
            memset(buffer, 0, sizeof(buffer));

            // Decoding is done inside the lock, so that the UI can't
            // delete the decoder while we're using it.
            lock();
            if (!current_stream.get()) current_stream = spooler.pop_next();

            if (current_stream.get()) {
                bytes_available = current_stream->read(buffer, sizeof(buffer));
                if (current_stream->eof) {
                    current_stream.reset();
                }
            }
            unlock();

            // Push audio from decoder to output. The way this is
            // written, gapless playback should "just work", assuming
            // the MP3 library behaves as expected.

            if (!ao_play(device, reinterpret_cast<char*>(buffer), bytes_available)) {
                // Audio device error.
                break;
            }
        }

        current_stream.reset();
    }

    static void* thread_main (void *arg) {
        static_cast<AudioThread*>(arg)->run();
        return NULL;
    }

} audio_thread;

// ------------------------------------------------------------
// Song library

// Song library, and current search selection.
vector<Song*> library, selection;

// Set of paths added to library, to avoid adding files twice
// (e.g. via symlinks).
set<string> unique_paths;

bool match_extension (string const& filename, const char *ext)
{
    const char *tail = std::strrchr(filename.c_str(), '.');
    return (tail && !strcasecmp(tail+1,ext));
}

string realpath_string (string const& path)
{
    // This use of realpath is not strict POSIX, but it's a
    // supported extension on Linux and OS X, at least.
    char *real = realpath(path.c_str(), NULL);
    if (real) {
        string result(real);
        free(real);
        return result;
    } else return path;
}

Maybe<string> id3v1string (char field[30])
{
    char buffer[31];
    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, field, 30);
    if (buffer[0]) return Just<string>(buffer);
    else return Nothing<string>();
}

Maybe<string> id3v2_string (mpg123_string *text)
{
    if (text && text->p) return Just<string>(text->p);
    else return Nothing<string>();
}

Maybe<int> id3v2_track_number (mpg123_id3v2 *v2)
{
    for (size_t i=0; i<v2->texts; i++) {
        mpg123_text const& text = v2->text[i];
        if ((text.id[0] == 'T') && (text.id[1] == 'R') &&
            (text.id[2] == 'C') && (text.id[3] == 'K') &&
            (text.text.p != NULL)) {
            int n = std::atoi(text.text.p);
            if ((n > 0) && (n <= 99)) return Maybe<int>(n);
        }
    }
    return Nothing<int>();
}

// Verify MP3 file is readable, scan ID3 tags, and return a Song
// object for the file.
Song* scan_mp3_file (string const& pathname, string const& filename)
{
    mpg123_handle *mh = mpg123_new(NULL,NULL);
    assert(mh != NULL);
    mpg123_param(mh, MPG123_ADD_FLAGS, MPG123_QUIET, 0.0);

    if (MPG123_OK == mpg123_open(mh,pathname.c_str())) {
        Song *song = new Song(pathname,filename);
        cout << "Scanning " << pathname << endl;

        // Must call this first, or the tags won't have been read yet:
        // mpg123_id3 doesn't actively seek them out, you have to get
        // the decoder going.
        mpg123_getformat(mh,NULL,NULL,NULL);

        mpg123_id3v1 *v1 = NULL;
        mpg123_id3v2 *v2 = NULL;

        if (MPG123_OK == mpg123_id3(mh, &v1, &v2)) {
            if (v2) {
                song->artist |= id3v2_string(v2->artist);
                song->title  |= id3v2_string(v2->title);
                song->album  |= id3v2_string(v2->album);
                song->track  |= id3v2_track_number(v2);
            }

            if (v1) {
                song->artist |= id3v1string(v1->artist);
                song->album  |= id3v1string(v1->album);
                song->title  |= id3v1string(v1->title);
            }
        }

        mpg123_delete(mh);
        return song;
    } else {
        mpg123_delete(mh);
        return NULL;
    }
}

// Scan file tags and add Song to library. Ensures duplicates are not
// added to the library, via unique_paths set.
int scan_file (string const& pathname, const char *filename)
{
    Song *s = NULL;
    string real = realpath_string(pathname);

    if (unique_paths.count(real)) {
        // cout << "Skipping duplicate " << real << endl;
        return 0;
    }

    if (match_extension(pathname,"mp3"))
        s = scan_mp3_file(real,filename);

    if (s) {
        library.push_back(s);
        unique_paths.insert(real);
        return 1;
    } else return 0;
}

// Walk directory tree and add song files to the library.
int scan_recursively (string const& path)
{
    int num_scanned = 0;
    DIR *dir = opendir(path.c_str());

    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir))) {
            string filename = path + "/" + de->d_name;
            struct stat s;
            if (!stat(filename.c_str(), &s)) {
                if (S_ISDIR(s.st_mode) && (de->d_name[0] != '.')) {
                    num_scanned += scan_recursively(filename.c_str());
                } else if (S_ISREG(s.st_mode)) {
                    num_scanned += scan_file(filename,de->d_name);
                }
            }
        }
        closedir(dir);
    } else cout << "Can't open \"" << path << "\"\n";

    return num_scanned;
}

bool song_library_ordering (Song *a, Song *b)
{
    return a->pathname < b->pathname;
}

void scan_expanding_path (string const& arg)
{
    int n = 0;
    wordexp_t expansion;
    memset(&expansion, 0, sizeof(expansion));
    int err = wordexp(arg.c_str(), &expansion, 0);

    // Valgrind shows some curious use of uninitialized data inside
    // wordexp on my Mac, and anecdotally, wordexp on Snow Leopard
    // might actually be buggy, so program extra defensively here:
    bool okay = !err && expansion.we_wordv && expansion.we_wordv[0];
    string path = okay? expansion.we_wordv[0] : arg;
    wordfree(&expansion);

    cout << "Scanning " << path << endl;
    n += scan_recursively(path);

    sort(library.begin(), library.end(), song_library_ordering);
    selection = library;

    cout << "Added " << n << " files.\n";
}

// ------------------------------------------------------------
// Library cache
//
// CacheWriter and CacheReader implement typed streams of data,
// consisting of strings and unsigned integers. The song cache is
// implemented as an alternating sequence of integer tags and string
// or integer values according to the tag.

namespace cached {
    // Magic type tags used in the cache file:
    enum atom { song=0x1230, artist=0x1231, album=0x1232, title=0x1233, track=0x1234 };
};


class CacheWriter {
protected:
    ofstream stream;

    void write_type (char type) {
        stream.write(&type, sizeof(type));
    }

    void write_word (unsigned x) {
        x = htonl(x);
        assert(sizeof(x == 4));
        stream.write(reinterpret_cast<const char *>(&x), sizeof(x));
    }

public:
    CacheWriter(const char *filename) :
        stream(filename, ios::out | ios::binary | ios::trunc)
    {}

    bool good () { return stream.good(); }

    CacheWriter& uint (unsigned x) {
        write_type('u');
        write_word(x);
        return *this;
    }

    CacheWriter& str (const char *ptr) {
        write_type('s');
        write_word(strlen(ptr));
        stream.write(ptr, strlen(ptr));
        return *this;
    }

    CacheWriter& str (string const& s) {
        return str(s.c_str());
    }

    CacheWriter& tagged (cached::atom tag, string const& value) {
        uint((unsigned)tag);
        str(value);
        return *this;
    }

    CacheWriter& tagged (cached::atom tag, unsigned value) {
        uint((unsigned)tag);
        uint(value);
        return *this;
    }

    template <typename T>
    CacheWriter& option (cached::atom tag, Maybe<T> const& opt) {
        if (opt.present) {
            tagged(tag, opt.value);
        }
        return *this;
    }
};

class CStreamException : public std::exception {
    const char *what () const throw() {
        return "Bad data in 'songs' cache";
    }
};

class CStreamTypeMismatch : public CStreamException {};
class CStreamGarbled : public CStreamException {};

class CacheReader {
protected:
    ifstream stream;
    vector<char> buffer;

    char read_type () {
        char x = 0;
        stream.read(&x, sizeof(x));
        return x;
    }

    unsigned read_word () {
        unsigned x = 0;
        assert(sizeof(x) == 4);
        stream.read(reinterpret_cast<char *>(&x), sizeof(x));
        x = ntohl(x);
        return x;
    }

public:
    CacheReader(const char *filename) :
        stream(filename, ios::in | ios::binary)
    {
        stream.exceptions(ifstream::failbit | ifstream::badbit | ifstream::eofbit);
    }

    bool good () {
        try {
            stream.peek();
        }
        catch (std::ios_base::failure) {
            return false;
        }

        return stream.good();
    }

    unsigned uint () {
        if (read_type() == 'u') return read_word();
        else throw CStreamTypeMismatch();
    }

    string str () {
        if (read_type() == 's') {
            unsigned length = read_word();
            buffer.resize(length);
            stream.read(&buffer[0], length);
            //string result;
            //result.assign(buffer.begin(), buffer.end());
            return string(buffer.begin(), buffer.end());
        } else throw CStreamTypeMismatch();
    }
};

string songs_path () {
    return prefpath("songs");
}

const char *song_cache_magic_string = "Shuffleclone++ song cache!";

void save_library_to_file (string const& filename)
{
    CacheWriter out(filename.c_str());
    if (out.good()) {
        out.str(song_cache_magic_string);
        for (auto s : library) {
            out.tagged(cached::song, s->pathname);
            out.str(s->filename);
            out.option(cached::artist, s->artist);
            out.option(cached::album, s->album);
            out.option(cached::title, s->title);
            out.option(cached::track, s->track);
        }
    } else cout << "Unable to write song cache to \"" << filename << "\"\n";
}

void load_library_from_file (string const& filename)
{
    try {
        CacheReader in(filename.c_str());

        if (in.str() != song_cache_magic_string) {
            cout << "Bogus 'songs' cache file. Rescan your music to fix this.\n";
            return;
        }

        Song *current_song = NULL;

        while (in.good()) {
            cached::atom tag = static_cast<cached::atom>(in.uint());

            switch (tag) {

            case cached::song: {
                string pathname = realpath_string(in.str());
                string filename = in.str();
                if (!unique_paths.count(pathname)) {
                    current_song = new Song(pathname,filename);
                    library.push_back(current_song);
                    unique_paths.insert(pathname);
                }
                continue;
            }

            case cached::artist: {
                string artist = in.str();
                if (current_song) current_song->artist = artist;
                continue;
            }

            case cached::album: {
                string album = in.str();
                if (current_song) current_song->album = album;
                continue;
            }

            case cached::title: {
                string title = in.str();
                if (current_song) current_song->title = title;
                continue;
            }

            case cached::track: {
                int track = in.uint();
                if (current_song) current_song->track = track;
                continue;
            }}

            // Written this way so the compiler can warn if I miss a
            // tag from the enumeration. Using 'default' would hide
            // that.
            cout << "Bad tag in library cache. Rescan your music to fix this.\n";
            return;
        }
    }
    catch (std::ios_base::failure)
    {
    }

    selection = library;
}

// ------------------------------------------------------------
// Player UI

bool running = true;

inline bool compare_case_insensitive (char a, char b) {
    return tolower(a) == tolower(b);
}

bool search_insensitive (string const& s, string const& substring) {
    return s.end() != search(s.begin(), s.end(),
                             substring.begin(), substring.end(),
                             compare_case_insensitive);
}

bool search_field (Maybe<string> const& field, string const& search_for) {
    return field.present && search_insensitive(field.value, search_for);
}

void refine_selection (string const& search_string)
{
    vector<Song*> new_selection;
    string munged = munge(search_string);

    for (auto song : selection)
    {
        if ((song->munged.find(munged.c_str()) != string::npos)
            || search_field(song->artist, search_string)
            || search_field(song->album,  search_string)
            || search_field(song->title,  search_string))
        {
            new_selection.push_back(song);
        }
    }
    selection = new_selection;
}

void print_selection ()
{
    int num = 1;
    for (auto s : selection)
    {
        printf("% 8i:  %s\n", num, s->pathname.c_str());

        if (s->artist.present) printf("      Artist: %s\n", s->artist.value.c_str());
        if (s->album.present)  printf("       Album: %s\n", s->album.value.c_str());
        if (s->title.present)  printf("       Title: %s\n", s->title.value.c_str());
        if (s->track.present)  printf("       Track: %i\n", s->track.value);

        num++;
    }
    fflush(stdout);
}

void play_songs (vector<Song*> const& songs)
{
    spooler.prequeue(songs);
    audio_thread.stop();
}

// Print time in seconds, in format [hh:]mm:ss
string format_seconds (int n) {
    char buf[32];
    if (n < 3600) snprintf(buf, sizeof(buf), "%i:%02i", n/60, n%60);
    else snprintf(buf, sizeof(buf), "%i:%02i:%02i", n/3600, (n%3600)/60, n%60);
    return buf;
}

// Test cases for time formatting.
struct seconds_format_test {
    class test_failure : public std::exception {};
    void test (int n, const char *match) {
        string s = format_seconds(n);
        //cout << s << " " << match << endl;
        if (s != match) {
            std::cerr << n << " printed as " << s << ", expected " << match << endl;
            throw test_failure();
        }
    }
    seconds_format_test() {
        test(  0,"0:00"), test(  1,"0:01"), test(  9,"0:09"), test( 10,"0:10");
        test( 11,"0:11"), test( 19,"0:19"), test( 20,"0:20"), test( 59,"0:59");
        test( 60,"1:00"), test( 61,"1:01"), test( 69,"1:09"), test( 70,"1:10");
        test( 71,"1:11"), test(119,"1:59"), test(120,"2:00"), test(121,"2:01");
        test(129,"2:09"), test(130,"2:10"), test(131,"2:11"), test(179,"2:59");
        test(180,"3:00"), test(599,"9:59"), test(600,"10:00"),test(601,"10:01");
        test(609,"10:09"),test(610,"10:10"),test(611,"10:11"),test(659,"10:59");
        test(660,"11:00"),test(661,"11:01"),test(670,"11:10"),test(690,"11:30");
        test(3540,"59:00"), test(3543,"59:03"), test(3597,"59:57");
        test(3600,"1:00:00"), test(3601,"1:00:01"), test(3610,"1:00:10");
        test(3650,"1:00:50"), test(3659,"1:00:59"), test(3660,"1:01:00");
        test(3669,"1:01:09"), test(3719,"1:01:59"), test(3720,"1:02:00");
        test(3721,"1:02:01"), test(3779,"1:02:59"), test(4199,"1:09:59");
        test(4200,"1:10:00"), test(4209,"1:10:09"), test(4210,"1:10:10");
        test(7199,"1:59:59"), test(7200,"2:00:00"), test(37230, "10:20:30");
    }
} _seconds_format_test_;

void describe_metadata (Song *s)
{
    if (s->artist.present) cout << "      Artist: " << s->artist.value << endl;
    if (s->album.present)  cout << "       Album: " << s->album.value << endl;
    if (s->title.present)  cout << "       Title: " << s->title.value << endl;
    if (s->track.present)  cout << "       Track: " << s->track.value << endl;
}

void now_playing ()
{
    Maybe<AudioThread::state_description> state = audio_thread.current_state();
    if (state.present) {
        Song *s = state.value.current_song;
        cout << " [" << format_seconds(state.value.time) << "/"
             << format_seconds(state.value.length) << "] "
             << "Now playing: " << s->pathname << endl;
        describe_metadata(state.value.current_song);
    } else {
        cout << "Not playing.\n";
    }
}

void play_random ()
{
    if (selection.size() > 0) {
        vector<Song*> v;
        v.push_back(selection[rand() % selection.size()]);
        play_songs(v);
        cout << "Playing " << v[0]->pathname << endl;
        describe_metadata(v[0]);
    }
}

Maybe<int> digit_char (char c)
{
    if ((c >= '0') && (c <= '9')) return Just<int>(c-'0');
    else return Nothing<int>();
}

vector<int> parse_ranges (const char *args, int minimum, int maximum)
{
    const char *ptr = args;
    vector<int> choices;
    Maybe<int> lower = Nothing<int>();
    Maybe<int> accumulator = Nothing<int>();

    do {
        Maybe<int> digit = digit_char(*ptr);
        if (digit.present) {
            accumulator = digit.value + 10 * (accumulator or 0);
        } else if (*ptr == '-') {
            lower = accumulator or minimum;
            accumulator.reset();
        } else {
            lower = lower or accumulator;
            Maybe<int> upper = accumulator or (lower.present? Just(maximum) : Nothing<int>());

            if (lower.present && upper.present) {
                for (int i=lower.value; i<=upper.value; i++) {
                    if ((i >= minimum) && (i <= maximum))
                        choices.push_back(i);
                }
            }

            accumulator.reset();
            lower.reset();
        }
    } while (*ptr++);

    return choices;
}

vector<Song*> parse_selected (const char *args)
{
    auto choices = parse_ranges(args, 1, selection.size());
    vector<Song *> songs;
    for (auto n : choices) {
        assert(n >= 1);
        assert((unsigned)n <= selection.size());
        songs.push_back(selection[n-1]);
    }
    return songs;
}

void enqueue_selected (const char *args)
{
    spooler.enqueue(parse_selected(args));
}

void play_selected (const char *args)
{
    play_songs(parse_selected(args));
}

void print_queue ()
{
    spooler.lock();
    int n = 1;
    auto queue = spooler.get_song_queue();
    for (auto song : queue)
    {
        printf("% 5i -- %s\n", n++, song->pathname.c_str());
    }
    fflush(stdout);
    spooler.unlock();
}

void drop_command (const char *args)
{
    spooler.lock();
    auto d = spooler.get_song_queue();
    auto choices = parse_ranges(args,1,d.size());
    set<int> choice_set(choices.begin(),choices.end());

    deque<Song*> newqueue;
    int index = 1;
    for (deque<Song*>::iterator i=d.begin(); i!=d.end(); ++i) {
        if (!choice_set.count(index)) {
            newqueue.push_back(*i);
        }
        index++;
    }
    spooler.set_song_queue(newqueue);
    spooler.unlock();
}


template <typename T>
struct parsed {
    Maybe<T> result;
    const char *rest;
};

parsed<long> parse_long (const char *s, int base = 10) {
    char *endptr;
    errno = 0;
    long x = std::strtol(s, &endptr, base);
    if (!errno && (endptr != s)) return (parsed<long>){Just<long>(x),endptr};
    else {
        cout << "nothing for you.\n";
        return (parsed<long>){Nothing<long>(),endptr};
    }
}

void seek_current (string const& arg)
{
    parsed<long> p = parse_long(arg.c_str());
    if (p.result.present) {
        seek_command cmd = {seek_command::absolute, p.result.value};
        if ((arg[0] == '+') || (arg[0] == '-')) cmd.mode = seek_command::relative;
        audio_thread.seek(cmd);
    }
}

void dispatch_command (string cmd)
{
    size_t split = cmd.find(' ');
    string name = cmd.substr(0, split);
    string args = "";
    bool has_args = false;

    if (split != string::npos)
        split = cmd.find_first_not_of(' ',split);

    if (split != string::npos) {
        args = cmd.substr(split);
        has_args = true;
    }

    if (cmd.length() <= 0) {
        selection = library;
        return;
    }

    if (name == "scan") {
        if (!has_args) cout << "Usage: scan <path to files>\n";
        else {
            scan_expanding_path(args);
            save_library_to_file(songs_path());
        }
    }
    else if (name == "ls") print_selection();
    else if (name == "quit") running = false;
    else if (name == "pause") audio_thread.toggle_pause();
    else if (name == "random") play_random();
    else if (name == "shuffle") random_shuffle(selection.begin(),selection.end());
    else if (name == "now") now_playing();
    else if (name == "seek") seek_current(args);
    else if (name == "queue") print_queue();
    else if (name == "next") audio_thread.stop();
    else if (name == "drop") drop_command(args.c_str());
    else if (name == "pre") spooler.prequeue(parse_selected(args.c_str()));
    else if ((cmd[0] == '-') || (std::isdigit(cmd[0]))) play_selected(cmd.c_str());
    else if (cmd[0] == '+') enqueue_selected(cmd.c_str()+1);
    else if (cmd[0] == '/') {
        refine_selection(cmd.substr(1));
        print_selection();
    } else if (cmd.length() > 0) {
        cout << "Unknown command \"" << name << "\"\n";
    }
}

void print_prompt ()
{
    if (selection.size() == library.size()) cout << "library> ";
    else cout << selection.size() << " matches> ";
}

void read_and_execute_command ()
{
    string cmd;
    print_prompt();
    std::getline(cin,cmd);
    dispatch_command(cmd);
}

void free_library ()
{
    for (auto song : library) delete song;
}

int main (int argc, char *argv[])
{
    srand(time(NULL));
    mpg123_init();
    ao_initialize();
    spooler.start();
    if (!audio_thread.start()) return 1;

    load_library_from_file(songs_path());

    if (library.size()) {
        cout << "This is Shuffleclone.\n";
        cout << library.size() << " songs in library." << endl;
    } else {
        cout << "Welcome to Shuffletron! To get started, scan some folders into\n";
        cout << "your library using the 'scan' command. For example:\n\n";
        cout << "  scan ~/music\n\n";
    }

    for (int i=1; i<argc; i++) {
        dispatch_command(argv[i]);
    }

    while (cin.good() && running) {
        read_and_execute_command();
    }

    cout << "Goodbye.\n";

    audio_thread.shutdown();
    spooler.shutdown();
    free_library();
    ao_shutdown();
    mpg123_exit();

    return 0;
}
