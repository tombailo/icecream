#include "config.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

#ifdef __FreeBSD__
// Grmbl  Why is this needed?  We don't use readv/writev
#include <sys/uio.h>
#endif

#include <fcntl.h>
#include <signal.h>
#include <assert.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <map>

#include <comm.h>
#include "client.h"

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

using namespace std;

string get_absfilename( const string &_file )
{
    string file;

    assert( !_file.empty() );
    if ( _file.at( 0 ) != '/' ) {
        static char buffer[PATH_MAX];

#ifdef HAVE_GETCWD
        getcwd(buffer, sizeof( buffer ) );
#else
        getwd(buffer);
#endif
        buffer[PATH_MAX - 1] = 0;
        file = string( buffer ) + '/' + _file;
    } else {
        file = _file;
    }

    string::size_type idx = file.find( "/.." );
    while ( idx != string::npos ) {
        file.replace( idx, 3, "/" );
        idx = file.find( "/.." );
    }
    idx = file.find( "/." );
    while ( idx != string::npos ) {
        file.replace( idx, 2, "/" );
        idx = file.find( "/." );
    }
    idx = file.find( "//" );
    while ( idx != string::npos ) {
        file.replace( idx, 2, "/" );
        idx = file.find( "//" );
    }
    return file;
}

static Service *get_server_for_job( CompileJob &job,  MsgChannel *scheduler, bool &got_env )
{
    Msg * umsg = scheduler->get_msg();
    if (!umsg || umsg->type != M_USE_CS)
    {
        log_error() << "replied not with use_cs " << ( umsg ? ( char )umsg->type : '0' )  << endl;
        delete umsg;
        throw( 1 );
    }
    UseCSMsg *usecs = dynamic_cast<UseCSMsg *>(umsg);
    string hostname = usecs->hostname;
    unsigned int port = usecs->port;
    int job_id = usecs->job_id;
    got_env = usecs->got_env;
    job.setJobID( job_id );
    job.setEnvironmentVersion( usecs->environment ); // hoping on the scheduler's wisdom
    trace() << "Have to use host " << hostname << ":" << port << " - Job ID: " << job.jobID() << " " << getpid() << "\n";
    delete usecs;

    Service *serv = new Service (hostname, port);

    trace() << getpid() << " service is there " << serv->channel() << endl;
    MsgChannel *cserver = serv->channel();
    if ( ! cserver ) {
        log_warning() << "no server found behind given hostname " << hostname << ":" << port << endl;
        throw ( 2 );
    }

    return serv;
}

static int build_remote_int(CompileJob &job, Service *serv, const string &version_file,
                            bool got_env, bool output )
{
    MsgChannel *cserver = serv->channel();

    unsigned char buffer[100000]; // some random but huge number

    trace() << "got environment " << ( got_env ? "true" : "false" ) << endl;

    off_t offset = 0;

    if ( !got_env ) {
        if ( ::access( version_file.c_str(), R_OK ) ) {
            log_error() << "$ICECC_VERSION has to point to an existing file to be installed\n";
            throw ( 3 );
        }
        // transfer env
        struct stat buf;
        if ( stat( version_file.c_str(), &buf ) ) {
            perror( "stat" );
            throw( 4 );
        }

        FILE *file = fopen( version_file.c_str(), "rb" );
        if ( !file )
            throw( 5 );

        EnvTransferMsg msg( job.environmentVersion() );
        if ( !cserver->send_msg( msg ) )
            throw( 6 );

        offset = 0;

        do {
            ssize_t bytes = fread(buffer + offset, 1, sizeof(buffer) - offset, file );
            offset += bytes;
            if (!bytes || offset == sizeof( buffer ) ) {
                if ( offset ) {
                    FileChunkMsg fcmsg( buffer, offset );
                    if ( !cserver->send_msg( fcmsg ) ) {
                        log_info() << "write of source chunk failed " << offset << " " << bytes << endl;
                        fclose( file );
                        throw( 7 );
                    }
                    offset = 0;
                }
                if ( !bytes )
                    break;
            }
        } while (1);
        fclose( file );

        if ( !cserver->send_msg( EndMsg() ) ) {
            log_info() << "write of end failed" << endl;
            throw( 8 );
        }
    }

    CompileFileMsg compile_file( &job );
    if ( !cserver->send_msg( compile_file ) ) {
        log_info() << "write of job failed" << endl;
        delete serv;
        throw( 9 );
    }

    int sockets[2];
    if (pipe(sockets)) {
        /* for all possible cases, this is something severe */
        exit(errno);
    }

    pid_t cpp_pid = call_cpp(job, sockets[1] );
    if ( cpp_pid == -1 )
        throw( 10 );
    close(sockets[1]);

    offset = 0;

    do {
        ssize_t bytes = read(sockets[0], buffer + offset, sizeof(buffer) - offset );
        offset += bytes;
        if (!bytes || offset == sizeof( buffer ) ) {
            if ( offset ) {
                FileChunkMsg fcmsg( buffer, offset );
                if ( !cserver->send_msg( fcmsg ) ) {
                    log_info() << "write of source chunk failed " << offset << " " << bytes << endl;
                    close( sockets[0] );
                    kill( cpp_pid, SIGTERM );
                    throw( 11 );
                }
                offset = 0;
            }
            if ( !bytes )
                break;
        }
    } while (1);

    int status = 255;
    waitpid( cpp_pid, &status, 0);

    if ( status ) { // failure

        // cserver->send_msg( CancelJob( job_id ) );
        delete cserver;
        cserver = 0;

        return WEXITSTATUS( status );
    }

    if ( !cserver->send_msg( EndMsg() ) ) {
        log_info() << "write of end failed" << endl;
        throw( 12 );
    }

    Msg *msg = cserver->get_msg();
    if ( !msg )
        throw( 14 );

    if ( msg->type != M_COMPILE_RESULT )
        throw( 13 );

    CompileResultMsg *crmsg = dynamic_cast<CompileResultMsg*>( msg );
    assert ( crmsg );

    status = crmsg->status;
    if ( output ) {
        fprintf( stdout, "%s", crmsg->out.c_str() );
        fprintf( stderr, "%s", crmsg->err.c_str() );
    }

    assert( !job.outputFile().empty() );

    if( status == 0 ) {
        string tmp_file = job.outputFile() + "_icetmp";
        int obj_fd = open( tmp_file.c_str(), O_CREAT|O_TRUNC|O_WRONLY|O_LARGEFILE, 0666 );

        if ( obj_fd == -1 ) {
            log_error() << "open failed\n";
            return EXIT_DISTCC_FAILED;
        }

        while ( 1 ) {
            msg = cserver->get_msg();
            if ( msg->type == M_END )
                break;

            if ( msg->type != M_FILE_CHUNK )
                return EXIT_PROTOCOL_ERROR;

            FileChunkMsg *fcmsg = dynamic_cast<FileChunkMsg*>( msg );
            if ( write( obj_fd, fcmsg->buffer, fcmsg->len ) != ( ssize_t )fcmsg->len )
                return EXIT_DISTCC_FAILED;
        }

        if( close( obj_fd ) == 0 )
            rename( tmp_file.c_str(), job.outputFile().c_str());
        else
            unlink( tmp_file.c_str());
    }
    return status;
}

int build_remote(CompileJob &job, MsgChannel *scheduler )
{
    char *get = getenv( "ICECC_VERSION");
    string version_file = "*";
    if ( get )
        version_file = get;

    int torepeat = 1;

    string version = version_file;
    string suff = ".tar.bz2";

    if ( version.size() > suff.size() && version.substr( version.size() - suff.size() ) == suff )
    {
        version = find_basename( version.substr( 0, version.size() - suff.size() ) );
    }

    GetCSMsg getcs (version, get_absfilename( job.inputFile() ), job.language(), torepeat );
    if (!scheduler->send_msg (getcs)) {
        log_error() << "asked for CS\n";
        throw( 0 );
    }

    if ( scheduler->protocol < 5 )
        torepeat = 1;

    if ( torepeat == 1 ) {
        bool got_env = false;
        Service *serv = get_server_for_job( job, scheduler, got_env );
        return build_remote_int( job, serv, version_file, got_env, true );
    } else {
        pid_t first;
        map<pid_t, int> jobmap;
        Service **servs = new Service*[torepeat];
        CompileJob *jobs = new CompileJob[torepeat];
        bool *got_envs = new bool[torepeat];

        for ( int i = 0; i < torepeat; i++ ) {
            jobs[i] = job;
            char buffer[PATH_MAX];
            if ( i ) {
                sprintf( buffer,  "/tmp/icecream_file_%02d", i );
                jobs[i].setOutputFile( buffer );
            } else
                sprintf( buffer, job.outputFile().c_str() );
            servs[i] = get_server_for_job( jobs[i], scheduler, got_envs[i] );
        }

        for ( int i = 0; i < torepeat; i++ ) {
            pid_t pid = fork();
            if ( !pid ) {
                int ret = -1;
                try {
                    ret = build_remote_int( jobs[i], servs[i], version_file, got_envs[i], i == 0 );
                } catch ( int error ) {
                    delete scheduler;
                    scheduler = 0;
                    ret = build_local( jobs[i], 0 );
                }
                ::exit( ret );
                return 0; // doesn't matter
            } else {
                if ( !i )
                    first = pid;
                jobmap[pid] = i;
            }
        }
        int status;
        int exit_code = -1;
        for ( int i = 0; i < torepeat; i++ ) {
            pid_t pid = wait( &status );
            if ( pid < 0 ) {
                perror( "wait failed" );
                status = -1;
            } else {
                if ( pid == first )
                    exit_code = status;
                printf( "file %s compiled with %d\n", jobs[jobmap[pid]].outputFile().c_str(), status );
            }
        }

        return exit_code;
    }
}
