// ------------------------------------------------------------
// Shuffleclone -- Shuffletron in C++(03).
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
#include <fstream>
#include <sstream>
#include <memory>

#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#include <ao/ao.h>
#include <mpg123.h>
#include <pthread.h>

using std::cout;
using std::cin;
using std::endl;
using std::ifstream;
using std::printf;
using std::string;
using std::vector;
using std::deque;
using std::set;
using std::auto_ptr;

// ------------------------------------------------------------
// Supporting definitions

// Simple Haskell-inspired option type.
template <typename T>
struct Maybe {
    bool present;
    T value;

    // I could (and probably should) define these constructors, but
    // I'd prefer to retain POD semantics. I could be persuaded
    // otherwise.
    // Maybe() : present(false) {}
    // Maybe(T value) : present(true), value(value) {}

    Maybe<T>& operator= (T const& new_value) {
        value = new_value;
        present = true;
        return *this;
    }

    void set (T t) { value = t; present = true; }
    void reset () { present = false; }

    // Defaulting value: (Maybe<T> || T) => T
    T operator|| (T alternative) const {
        return present? value : alternative;
    }

    // Combining Maybes: (Maybe<T> || Maybe<T>) => Maybe<T>
    Maybe<T> const& operator|| (Maybe<T> const& alternative) const {
        return present? *this : alternative;
    }
};

template <typename T>
Maybe<T> Just (T v) { return (Maybe<T>){true,v}; }

template <typename T>
Maybe<T> Nothing () { return (Maybe<T>){false}; }

// Normalize case and remove (most) punctuation, for search queries.
string munge (string const& s, int strip_tail=0)
{
    std::ostringstream munged;
    int n = s.length() - strip_tail;
    if (n <= 0) return "";
    for (int in=0; in<n; in++) {
        char c = std::tolower(s[in]);
        if (std::isalnum(c) || c=='/') munged << c;
    }

    return munged.str();
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

    Song(string const& pathname, string const& filename)
        : pathname(pathname),
          filename(filename),
          munged(munge(pathname,4))
    { }
};

// A song_stream is instantiated to play a particular song. Created by
// the spooler, consumed by the audio thread.
struct song_stream
{
    Song *song;
    mpg123_handle *mh;
    bool paused;
    bool eof;
    long rate;
    int channels;

    song_stream(Song *song, mpg123_handle *mh)
        : song(song), mh(mh), paused(false), eof(false)
    {
        int encoding;
        mpg123_getformat(mh,&rate,&channels,&encoding);
    }

    size_t read (unsigned char *buffer, size_t size)
    {
        if (eof) return 0;
        else if (paused) return size;
        else {
            size_t bytes_read;
            int code = mpg123_read(mh, buffer, size, &bytes_read);
            switch (code) {
            case MPG123_DONE:
                eof = true;
                return bytes_read;

            case MPG123_OK:
                return bytes_read;

            default:
                printf("%s\n", mpg123_strerror(mh));
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
// Playback queue

class Spooler
{
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;

    deque<Song *> song_queue;
    bool running;
    auto_ptr<song_stream> preloaded;

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
        typedef vector<Song*>::const_reverse_iterator iter;
        lock();
        for (iter i=songs.rbegin(); i!=songs.rend(); ++i) {
            song_queue.push_front(*i);
        }
        unlock();
    }

    void enqueue (vector<Song*> const& songs) {
        lock();
        for (vector<Song*>::const_iterator i=songs.begin(); i!=songs.end(); ++i) {
            song_queue.push_back(*i);
        }
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
    auto_ptr<song_stream> pop_next () {
        if (!pthread_mutex_trylock(&mutex)) {

            auto_ptr<song_stream> popped;
            popped = preloaded;

            if (popped.get()) {
                assert(popped->song == song_queue.front());
                song_queue.pop_front();
            }

            unlock();
            return popped;
        } else return auto_ptr<song_stream>();
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
                        mpg123_open(mh,song->pathname.c_str());
                        mpg123_scan(mh);

                        preloaded.reset(new song_stream(song,mh));
                    } else {
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

    auto_ptr<song_stream> current_stream;

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
            printf("Using audio driver \"%s\"\n", device_name);
            assert(0==pthread_create(&thread, NULL, thread_main, this));
            return true;
        } else {
            printf("Failed to open audio device \"%s\"\n", device_name);
            return false;
        }
    }

    void play (auto_ptr<song_stream> new_stream) {

        // Atomically swap the current stream with the new one..
        lock();
        auto_ptr<song_stream> old_stream;
        old_stream = current_stream;
        current_stream = new_stream;
        unlock();

        // When we exit this scope, the old_stream gets freed by the
        // auto_ptr. I specifically wanted this to occur outside the
        // lock, so as to not block the audio thread, in case for some
        // reason it takes longer than it should.
    }

    void stop () {
        play(auto_ptr<song_stream>());
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

    Song* current_song () {
        Song *song = NULL;

        lock();
        if (current_stream.get()) song = current_stream->song;
        unlock();

        return song;
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
            // written, gapless playback should "just work", but I
            // haven't verified it.

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
// Player UI

bool running = true;
vector<Song*> library, selection;

string preferences_root ()
{
    const char *home = std::getenv("HOME");
    assert(home != NULL);
    string root = home + string("/.shuffleclone");
    mkdir(root.c_str(), 0700);
    return home;
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

bool match_extension (string const& filename, const char *ext)
{
    const char *tail = std::strrchr(filename.c_str(), '.');
    return (tail && !strcasecmp(tail+1,ext));
}

int scan_file (string const& pathname, const char *filename)
{
    if (match_extension(pathname,"mp3")) {
        //cout << "Added " << pathname << endl;
        library.push_back(new Song(pathname,filename));
        return 1;
    } else return 0;
}

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
    } else printf("Can't open %s\n", path.c_str());

    closedir(dir);
    return num_scanned;
}

bool song_compare (Song *a, Song *b)
{
    return a->pathname < b->pathname;
}

void scan_path (string path)
{
    cout << "Scanning " << path << endl;
    int n = scan_recursively(path.c_str());
    sort(library.begin(), library.end(), song_compare);

    cout << "Added " << n << " files.\n";
}

void refine_selection (string const& search_string)
{
    vector<Song*> new_selection;
    string munged = munge(search_string);

    for (vector<Song*>::iterator i=selection.begin(); i!=selection.end(); ++i)
    {
        Song *song = *i;
        if (song->munged.find(munged.c_str()) != string::npos) {
            new_selection.push_back(song);
        }
    }
    selection = new_selection;
}

void print_selection ()
{
    int num = 1;
    for (vector<Song*>::iterator i=selection.begin(); i!=selection.end(); ++i)
    {
        printf("% 8i:  %s\n", num, (*i)->pathname.c_str());
        num++;
    }
}

// Ensure path is absolute. Relative paths are taken relative to the CWD.
string absolutize (string const& path)
{
    char buf[1024] = "";
    getcwd(buf,sizeof(buf));
    if ((path.size() > 0) && (path[0] == '/')) return path;
    else return string(buf) + "/" + path;
}

void play_songs (vector<Song*> const& songs)
{
    spooler.prequeue(songs);
    audio_thread.stop();
}

void now_playing ()
{
    Song *s = audio_thread.current_song();
    if (s != NULL) {
        cout << "Now playing: " << s->pathname << endl;
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
    vector<int> choices = parse_ranges(args, 1, selection.size());
    vector<Song *> songs;
    for (vector<int>::iterator i = choices.begin(); i!=choices.end(); ++i) {
        assert(*i >= 1);
        assert((unsigned)*i <= selection.size());
        songs.push_back(selection[*i-1]);
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
    deque<Song*> const& queue = spooler.get_song_queue();
    for (deque<Song*>::const_iterator i=queue.begin(); i!=queue.end(); ++i)
    {
        printf("% 5i -- %s\n", n++, (*i)->pathname.c_str());
    }
    spooler.unlock();
}

void drop_command (const char *args)
{
    spooler.lock();
    deque<Song*>& d = spooler.get_song_queue();
    vector<int> choices = parse_ranges(args,1,d.size());
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
            scan_path(absolutize(args));
            selection = library;
        }
    }
    else if (name == "ls") print_selection();
    else if (name == "quit") running = false;
    else if (name == "pause") audio_thread.toggle_pause();
    else if (name == "random") play_random();
    else if (name == "shuffle") random_shuffle(selection.begin(),selection.end());
    else if (name == "now") now_playing();
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
    for (vector<Song*>::iterator i=selection.begin(); i!=selection.end(); ++i)
    {
        delete *i;
    }
}

int main (int argc, char *argv[])
{
    srand(time(NULL));
    mpg123_init();
    ao_initialize();

    spooler.start();
    if (!audio_thread.start()) return 1;

    printf("This is Shuffleclone.\n");

    for (int i=1; i<argc; i++) {
        dispatch_command(argv[i]);
    }

    while (cin.good() && running) {
        read_and_execute_command();
    }

    printf("Goodbye.\n");

    audio_thread.shutdown();
    spooler.shutdown();

    free_library();
    ao_shutdown();
    mpg123_exit();

    return 0;
}
