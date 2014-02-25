/*
  This file implements the fully fledged util�abort() function which
  assumes that the current build has the following features:

    fork()      : To support calling external program addr2line().
    pthread     : To serialize the use of util�abort() - not very important.
    execinfo.h  : The backtrace functions backtrace() and backtrace�symbols().
    _GNU_SOURCE : To get the dladdr() function.

  If not all these features are availbale the simpler version in
  util�abort_simple.c is built instead.
*/

/**
  This function uses the external program addr2line to convert the
  hexadecimal adress given by the libc function backtrace() into a
  function name and file:line.

  Observe that the function is quite involved, so if util_abort() is
  called because something is seriously broken, it might very well fail.

  The executable should be found from one line in the backtrace with
  the function util_bt_alloc_current_executable(), the argument
  bt_symbol is the lines generated by the  bt_symbols() function.

  This function is purely a helper function for util_abort().
*/

#if !defined(__GLIBC__)         /* note: not same as __GNUC__ */
#  if defined (__APPLE__)
#    include <stdlib.h>         /* alloca   */
#    include <sys/syslimits.h>  /* PATH_MAX */
#    include <mach-o/dyld.h>    /* _NSGetExecutablePath */
#  elif defined (__LINUX__)
#    include <stdlib.h>         /* alloca   */
#    include <limits.h>         /* PATH_MAX */
#    include <unistd.h>         /* readlink */
#  else
#    error No known program_invocation_name in runtime library
#  endif
#endif

#define UNDEFINED_FUNCTION "??"

static bool util_addr2line_lookup__(const void * bt_addr , char ** func_name , char ** file_name , int * line_nr, bool subtract_base_adress) {
  *func_name = NULL;    // If dladdr() succeeds, but addr2line fails the func_name pointer will be set, but the function will return false anyway.
  *file_name = NULL;
  *line_nr   = 0;
  {
    bool  address_found = false;
    Dl_info dl_info;

    if (dladdr(bt_addr , &dl_info)) {
      const char * executable = dl_info.dli_fname;
      *func_name = util_alloc_string_copy( dl_info.dli_sname );
      if (util_file_exists( executable )) {
        char *stdout_file = util_alloc_tmp_file("/tmp" , "addr2line" , true);
        /* 1: Run addr2line application */
        {
          char ** argv = util_calloc(3 , sizeof * argv );
          argv[0] = util_alloc_string_copy("--functions");
          argv[1] = util_alloc_sprintf("--exe=%s" , executable );
          {
            char * rel_address = (char *) bt_addr;
            if (subtract_base_adress)
              rel_address -= (size_t) dl_info.dli_fbase;
            argv[2] = util_alloc_sprintf("%p" , (void *) rel_address);
          }
          util_fork_exec("addr2line" , 3  , (const char **) argv , true , NULL , NULL , NULL , stdout_file , NULL);
          util_free_stringlist(argv , 3);
        }
        
        /* 2: Parse stdout output */
        {
          bool at_eof;
          FILE * stream = util_fopen(stdout_file , "r");
          char * tmp_fname = util_fscanf_alloc_line(stream , &at_eof);

          if (strcmp(tmp_fname , UNDEFINED_FUNCTION) != 0) {
            char * stdout_file_name = util_fscanf_alloc_line(stream , &at_eof);
            char * line_string = NULL;
            util_binary_split_string( stdout_file_name , ":" , false , file_name , &line_string);
            if (line_string && util_sscanf_int( line_string , line_nr)) 
              address_found = true;
            
            free( stdout_file_name );
            util_safe_free( line_string );
          }
          free( tmp_fname );
          fclose(stream);
        }
        util_unlink_existing(stdout_file);
        free( stdout_file );
      } 
    } 
    return address_found;
  }
}



bool util_addr2line_lookup(const void * bt_addr , char ** func_name , char ** file_name , int * line_nr) {
  if (util_addr2line_lookup__(bt_addr , func_name , file_name , line_nr , false))
    return true;
  else
    return util_addr2line_lookup__(bt_addr , func_name , file_name , line_nr , true);
}


/**
  This function prints a message to stream and aborts. The function is
  implemented with the help of a variable length argument list - just
  like printf(fmt , arg1, arg2 , arg3 ...);

  Observe that it is __VERY__ important that the arguments and the
  format string match up, otherwise the util_abort() routine will hang
  indefinetely; without printing anything to stream.

  A backtrace is also included, with the help of the exernal utility
  addr2line, this backtrace is converted into usable
  function/file/line information (provided the required debugging
  information is compiled in).
*/

static pthread_mutex_t __abort_mutex  = PTHREAD_MUTEX_INITIALIZER; /* Used purely to serialize the util_abort() routine. */


static char * realloc_padding(char * pad_ptr , int pad_length) {
  int i;
  pad_ptr = util_realloc( pad_ptr , (pad_length + 1) * sizeof * pad_ptr );
  for (i=0; i < pad_length; i++)
    pad_ptr[i] = ' ';
  pad_ptr[pad_length] = '\0';
  return pad_ptr;
}



static void util_fprintf_backtrace(FILE * stream) {
  const char * with_linenr_format = " #%02d %s(..) %s in %s:%d\n";
  const char * func_format        = " #%02d %s(..) %s in ???\n";
  const char * unknown_format     = " #%02d ???? \n";

  const int max_bt = 50;
  const int max_func_length = 60;
  void *bt_addr[max_bt];
  int    size,i;

  size       = backtrace(bt_addr , max_bt);

  fprintf(stream , "--------------------------------------------------------------------------------\n");
  for (i=0; i < size; i++) {
    int line_nr;
    char * func_name;
    char * file_name;
    char * padding = NULL;
    
    if (util_addr2line_lookup(bt_addr[i] , &func_name , &file_name , &line_nr)) {
      int pad_length;
      char * function;
      // Seems it can return true - but with func_name == NULL?! Static/inlinded functions?
      if (func_name)
        function = func_name;
      else
        function = "???";

      pad_length = util_int_max (2, 2 + max_func_length - strlen(function));
      padding = realloc_padding( padding , pad_length);
      fprintf(stream , with_linenr_format , i , function , padding , file_name , line_nr);
    } else {
      if (func_name != NULL) {
        int pad_length = 2 + max_func_length - strlen(func_name);
        padding = realloc_padding( padding , pad_length);
        fprintf(stream , func_format , i , func_name , padding);
      } else {
        padding = realloc_padding( padding , 2 + max_func_length );
        fprintf(stream , unknown_format , i , padding);
      }
    }
    
    util_safe_free( func_name );
    util_safe_free( file_name );
    util_safe_free( padding );
  }
  fprintf(stream , "--------------------------------------------------------------------------------\n");
}

char * util_alloc_dump_filename() {
  time_t timestamp = time(NULL);
  char day[32];
  strftime(day, 32, "%Y%m%d-%H%M%S", localtime(&timestamp));
  {
    uid_t uid = getuid();
    struct passwd *pwd = getpwuid(uid);
    char * filename;
    
    if (pwd)
      filename = util_alloc_sprintf("/tmp/ert_abort_dump.%s.%s.log", pwd->pw_name, day);
    else
      filename = util_alloc_sprintf("/tmp/ert_abort_dump.%d.%s.log", uid , day);

    return filename;
  }
}


void util_abort(const char * fmt , ...) {
  pthread_mutex_lock( &__abort_mutex ); /* Abort before unlock() */
  {
    char * filename = NULL;
    FILE * abort_dump = NULL;

    if (!getenv("ERT_SHOW_BACKTRACE")) 
      filename = util_alloc_dump_filename();

    if (filename)
      abort_dump = fopen(filename, "w");
    
    if (abort_dump == NULL) 
      abort_dump   = stderr;
    
    va_list ap;

    va_start(ap , fmt);
    fprintf(abort_dump , "\n\n");
    vfprintf(abort_dump , fmt , ap);
    fprintf(abort_dump , "\n\n");
    va_end(ap);

    /*
      The backtrace is based on calling the external program
      addr2line; the call is based on util_fork_exec() which is
      currently only available on POSIX.
    */
    const bool include_backtrace = true;
    if (include_backtrace) {
      if (__abort_program_message != NULL) {
#if !defined(__GLIBC__)
        /* allocate a temporary buffer to hold the path */
        char* program_invocation_name = alloca (PATH_MAX);
#  if defined(__APPLE__)
        uint32_t buflen = PATH_MAX;
        _NSGetExecutablePath (program_invocation_name, &buflen);
#  elif defined(__LINUX__)
        readlink ("/proc/self/exe", program_invocation_name, PATH_MAX);
#  endif
#endif /* !defined(__GLIBC__) */
        fprintf(abort_dump,"--------------------------------------------------------------------------------\n");
        fprintf(abort_dump,"%s",__abort_program_message);
        fprintf(abort_dump, "Current executable ..: %s\n" , program_invocation_name);
        fprintf(abort_dump,"--------------------------------------------------------------------------------\n");
      }

      fprintf(abort_dump,"\n");
      util_fprintf_backtrace( abort_dump );
    }

    if (abort_dump != stderr) {
      util_fclose(abort_dump);
      fprintf(stderr, "\n\nA fatal error occured, see file: %s for details.\nSetting the environment variable \"ERT_SHOW_BACKTRACE\" will show the backtrace on stderr.\n", filename);
    }

    free(filename);
  }
  
  pthread_mutex_unlock(&__abort_mutex);
  signal(SIGABRT, SIG_DFL);
  abort();
}


/*****************************************************************/

