#include "chuck.h"
#include "chuck_globals.h"
#include "chuck_vm.h"
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <cstdlib>
#include <sys/stat.h>
#include <fcntl.h>
#include <unordered_map>
#include <iomanip> 
#include <chrono>


SAMPLE *g_outputBuffer = NULL;
SAMPLE *g_inputBuffer = NULL;
t_CKINT g_bufferSize = 256;
t_CKINT g_sampleRate = 48000;
t_CKINT g_numChannels = 2;
t_CKINT g_logLevel = 0;
t_CKUINT shredID = 0;
const char* g_cmdPipe = "/tmp/chuck_cmd";
int g_cmdFd = -1;
volatile bool g_exit_requested = false;

long vm_start_time;

void show_usage(const char *name) {
    std::cerr << "Usage: " << name << " [options] file.ck\n"
              << "Options:\n"
              << "  -r <rate>    Sample rate (default: 48000)\n"
              << "  -b <size>    Buffer size (default: 256)\n"
              << "  -c <chans>   Number of channels (default: 2)\n"
              << "  -l <level>   Log level (0-7)\n"
              << "  -h           Show this help message\n";
}

int parse_args(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h") {
            show_usage(argv[0]);
            return 0;
        } else if (arg == "-r" && i+1 < argc) {
            g_sampleRate = atoi(argv[++i]);
        } else if (arg == "-b" && i+1 < argc) {
            g_bufferSize = atoi(argv[++i]);
        } else if (arg == "-c" && i+1 < argc) {
            g_numChannels = atoi(argv[++i]);
        } else if (arg == "-l" && i+1 < argc) {
            int level = atoi(argv[++i]);
            g_logLevel = (level >= 0 && level <= 7) ? level : 0;
        } else if (arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << "\n";
            show_usage(argv[0]);
            return -1;
        }
    }
    return 1;
}

////////////////////
// COMMAND PIPING //
////////////////////

//-----------------------------------------------------------------------------
// VM message callback function (called by chuck VM in response to queue_msg())
//-----------------------------------------------------------------------------
void vm_msg_cb( const Chuck_Msg * msg )
{
    // check the type
    switch( msg->type )
    {
        // cases we'd like to handle
        case CK_MSG_STATUS:
        {
            // get status info
            Chuck_VM_Status * status = msg->status;
            // verify
            if( status )
            {
                // number shreds
                cerr << "\n-------------------------------" << endl;
                cerr << "printing from callback function" << endl;
                cerr << "-------------------------------" << endl;
                cerr << "# of shreds in VM: " << status->list.size() << endl;
                // chuck time (formatted as an integer to avoid scientific notation)
                cerr << "chuck time: " << std::fixed << std::setprecision(0) << status->now_system << "::samp ("
                     << status->t_hour << "h"
                     << status->t_minute << "m"
                     << status->t_second << "s)" << endl;

                // restore default formatting
                std::cout.unsetf(std::ios::fixed);
                std::cout.precision(6);

                // print status
                if( status->list.size() ) cerr << "--------"  << endl;
                for( t_CKUINT i = 0; i < status->list.size(); i++ )
                {
                    // get shred info
                    Chuck_VM_Shred_Status * info = status->list[i];
                    // print shred info
                    cerr << "[shred] id: " << info->xid
                         << " source: " << info->name.c_str()
                         << " spork time: " << (status->now_system - info->start) / status->srate
                         << " state: " << (!info->has_event ? "ACTIVE" : "(waiting on event)") << endl;
                }
                if( status->list.size() ) cerr << "--------"  << endl;

                // clean up
                CK_SAFE_DELETE( status );
            }
        }
        break;
    }
    // our responsibility to delete
    CK_SAFE_DELETE( msg );
}

void check_commands(ChucK* chuck) {
    char buffer[256];
    ssize_t bytes = read(g_cmdFd, buffer, sizeof(buffer)-1);
    Chuck_Msg *msg = NULL;
    Chuck_VM * VM = chuck->vm();
    if(bytes > 0) {
        buffer[bytes-1] = '\0';
        std::string cmd(buffer);
        // 'exit' command
        if(cmd == "exit")
        {
            std::cerr << "exit command received – shutting down VM …" << std::endl;
            g_exit_requested = true;   // tell main loop to stop
        }
        // { <<< "arbitrary_code" >>>; }
        else if(cmd.front() == '{' && cmd.back() == '}')
        {
            // remove braces
            std::string code = cmd.substr(1, cmd.size() - 2);
            // compile raw code string
            bool ok = chuck->compileCode(code.c_str(), "eval", 1, FALSE);
            if(ok) {
                shredID = chuck->vm()->last_id();
                std::cerr << "eval'd code block: " << cmd << std::endl;
            } else {
                std::cerr << "failed to eval code block" << std::endl;
            }
        }
        // add shred
        else if(cmd.find("+") == 0)
        {
            // get string
            std::string fullpath_with_args = cmd.substr(2); // remove "+ " 
            // filename
            std::string fullpath;
            // arguments
            std::string args;
            // extract args FILE:arg1:arg2:arg3
            extract_args( fullpath_with_args, fullpath, args );

            // extract the filename (e.g. "somefile.ck")
            size_t pos = fullpath.find_last_of("/\\");
            std::string filename = (pos != std::string::npos) ? fullpath.substr(pos + 1) : fullpath;

            // compile the file but don't run it yet (instance == 0)
            if( !VM->carrier()->chuck->compileFile( fullpath, args, 0 ) ) return;

            // construct chuck msg (must allocate on heap, as VM will clean up)
            Chuck_Msg * msg = new Chuck_Msg();
            // set type
            msg->type = CK_MSG_ADD;
            // set code
            msg->code = VM->carrier()->compiler->output();
            // create args array
            msg->args = new vector<string>;
            // extract args again but this time into vector
            extract_args( fullpath_with_args, fullpath, *(msg->args) );
            // process ADD message, return new shred ID
            t_CKINT shred_id = VM->process_msg(msg);
        }
        // remove shred
        else if(cmd.find("-") == 0)
        {
            // Parse shred ID
            size_t space_pos = cmd.find(' ');
            if(space_pos == std::string::npos) {
                std::cerr << "Invalid remove command format" << std::endl;
                return;
            }

            std::string id_str = cmd.substr(space_pos + 1);
            t_CKUINT shred_id;
            try {
                if(id_str.find("0x") == 0) {
                    shred_id = std::stoul(id_str, nullptr, 16);
                } else {
                    shred_id = std::stoul(id_str);
                }
            } catch(...) {
                std::cerr << "Invalid shred ID: " << id_str << std::endl;
                return;
            }
            // create a message; VM will delete
            msg = new Chuck_Msg;
            // message type
            msg->type = CK_MSG_REMOVE;
            // signify no value (will remove last)
            msg->param = shred_id;
            // queue on VM
            chuck->vm()->queue_msg(msg);
        }
        // prints VM status
        else if(cmd.find("^") == 0)
        {
            // create a message, we will delete later on
            msg = new Chuck_Msg;
            // message type
            msg->type = CK_MSG_STATUS;
            // create a status info
            msg->status = new Chuck_VM_Status();
            // add callback function
            msg->reply_cb = vm_msg_cb;
            // queue msg in VM; reply will be received by the callback
            chuck->vm()->queue_msg(msg);
        }
        else if(cmd.find("get_vm_start_time()") == 0)
        {
                // output the vm start time in microseconds
    		std::cerr << "VM start time in microseconds: " << vm_start_time << std::endl;
        }
        else
        {
            cerr << "*** unrecognized command: " << cmd << " ***" << endl;
        }

        // Flush the FIFO by draining any remaining data (non-blocking read)
        char buf[4096];
        while (read(g_cmdFd, buf, sizeof(buf)) > 0);
    }
}

////////////////////////
// END COMMAND PIPING //
////////////////////////

////////////////////////////
// Custom logger function //
////////////////////////////
static void chuck_logger(t_CKINT level, const std::string &message, void *userData)
{
    // Route all ChucK messages to stderr
    // Filter by log level
    if(g_logLevel == 0) return; // -l 0 = silent
    if(g_logLevel < level) return;  // CHECK THIS
    const char* prefixes[] = {
        " ",
        "LEVEL 1: ",
        "LEVEL 2: ",
        "LEVEL 3: ",
        "LEVEL 4: ",
        "LEVEL 5: ",
        "LEVEL 6: ",
        "LEVEL 7: ",
    };
    std::cerr << "[ChucK] " << prefixes[level] << message << std::endl;
}

#include <dirent.h>
#include <string.h>

////////////////////
// CHUGINS LOADER //
////////////////////
// Function to find all .chug files in a directory
void loadAllChugins(ChucK* chuck, const std::string& dirPath) {
    std::list<std::string> chuginPaths;
    DIR* dir = opendir(dirPath.c_str());

    if (!dir) {
        std::cerr << "Could not open Chugin directory: " << dirPath << std::endl;
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        std::string filename(entry->d_name);
        // Check if file ends with .chug
        if (filename.length() > 5 && 
            filename.substr(filename.length() - 5) == ".chug") {
            std::string fullPath = dirPath + "/" + filename;
            chuginPaths.push_back(fullPath);
            std::cerr << "Found Chugin: " << fullPath << std::endl;
        }
    }
    closedir(dir);

    if (!chuginPaths.empty()) {
        chuck->setParam(CHUCK_PARAM_USER_CHUGINS, chuginPaths);
    }
}

// get current epoch time in microseconds
long get_time() {
    // Get the current time point from the system_clock
    auto now = std::chrono::system_clock::now();

    // Get the duration since the epochs
    auto duration_since_epoch = now.time_since_epoch();

    // Cast the duration to microseconds
    auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration_since_epoch);

    return microseconds.count();
}


/////////////////////
// Ready, set, go! //
/////////////////////
int main(int argc, char** argv) {
	
    // Redirect stdout to stderr
    std::cout.rdbuf(std::cerr.rdbuf());  

    // Parse command line arguments
    int parse_result = parse_args(argc, argv);
    if (parse_result <= 0) return parse_result;

    // Open for *read only* (block until writer connects)
    mkfifo(g_cmdPipe, 0666); // Create named pipe
    g_cmdFd = open(g_cmdPipe, O_RDONLY);  // BLOCKS until writer connects

    // Initialize ChucK with configured parameters
    ChucK* chuck = new ChucK();

    // Set up logging
    ChucK::setLogLevel(g_logLevel);
    chuck->setChoutCallback([](const char* msg) { std::cerr << msg; });
    chuck->setCherrCallback([](const char* msg) { std::cerr << msg; });

    // Audio params
    chuck->setParam(CHUCK_PARAM_SAMPLE_RATE, g_sampleRate);
    chuck->setParam(CHUCK_PARAM_OUTPUT_CHANNELS, g_numChannels);
    chuck->setParam(CHUCK_PARAM_VM_HALT, FALSE);

    // Load chugins
    loadAllChugins(chuck, "/usr/local/lib/chuck");

    // Allocate buffers with proper channel count
    g_outputBuffer = new SAMPLE[g_bufferSize * g_numChannels];
    memset(g_outputBuffer, 0, g_bufferSize * g_numChannels * sizeof(SAMPLE));
    g_inputBuffer = new SAMPLE[g_bufferSize * g_numChannels];
    memset(g_inputBuffer, 0, g_bufferSize * g_numChannels * sizeof(SAMPLE));

    // Initialize and start
    chuck->init();
    chuck->start();
    
    // grab time of VM start in microseconds
    vm_start_time = get_time();

    // Check if there's a filename and if so, run it
    std::string lastArg = argc > 1 ? argv[argc-1] : "";
    bool hasScript = lastArg.length() > 3 && lastArg.substr(lastArg.length() - 3) == ".ck";

    if (hasScript) {
        if(!chuck->compileFile(lastArg.c_str())) {
            std::cerr << "Failed to compile file: " << lastArg << std::endl;
            exit(1);
        }
    }

    // setup of STDOUT for fwrite
    FILE *out = fdopen(STDOUT_FILENO, "wb"); // Open stdout as FILE* for buffered I/O
    setvbuf(out, NULL, _IOFBF, 0); // Set a buffer -- you can experiment with other vals than 0 (0 seems to work, tho)
    std::vector<int32_t> writeBuffer(g_bufferSize * g_numChannels);

    // MAIN LOOP
    while(chuck->vm_running() && !g_exit_requested) {
        // Respond to piped commands
        check_commands(chuck);
        // Run audio computation
        chuck->run(g_inputBuffer, g_outputBuffer, g_bufferSize);
        // Convert and write samples
        for (int x = 0; x < g_bufferSize * g_numChannels; x++) {
            float sample = g_outputBuffer[x];
            writeBuffer[x] = static_cast<int32_t>(sample * 2147483647.0f);
        }
        fwrite(writeBuffer.data(), sizeof(int32_t), writeBuffer.size(), out);
    }

    // Cleanup
    delete[] g_outputBuffer;
    delete[] g_inputBuffer;
    CK_SAFE_DELETE(chuck);
    close(g_cmdFd);
    unlink(g_cmdPipe);
    return 0;
}
 
