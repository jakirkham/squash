#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <strings.h>

#include <squash/squash.h>

static void
print_help_and_exit (int argc, char** argv, int exit_code) {
  fprintf (stderr, "Usage: %s [OPTION]... FILE...\n", argv[0]);
  fprintf (stderr, "Benchmark Squash plugins.\n");
  fprintf (stderr, "\n");
  fprintf (stderr, "Options:\n");
  fprintf (stderr, "\t-o outfile    Write data to outfile (default is stdout)\n");
  fprintf (stderr, "\t-h            Print this help screen and exit.\n");
  fprintf (stderr, "\t-c codec      Benchmark the specified codec and exit.\n");
  fprintf (stderr, "\t-f format     Output format.  One of:\n");
  fprintf (stderr, "\t                \"json\" (default)\n");
  fprintf (stderr, "\t                \"csv\"\n");

  exit (exit_code);
}

typedef enum _BenchmarkOutputFormat {
  BENCHMARK_OUTPUT_FORMAT_JSON,
  BENCHMARK_OUTPUT_FORMAT_CSV
} BenchmarkOutputFormat;

struct BenchmarkContext {
  FILE* output;
  FILE* input;
  char* input_name;
  bool first;
  long input_size;
  BenchmarkOutputFormat format;
};

struct BenchmarkTimer {
  struct timespec start_wall;
  struct timespec end_wall;
  struct timespec start_cpu;
  struct timespec end_cpu;
};

static void
benchmark_context_write_json (struct BenchmarkContext* context, const char* fmt, ...) {
  if (context->format == BENCHMARK_OUTPUT_FORMAT_JSON) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(context->output, fmt, ap);
    va_end(ap);
  }
}

static void
benchmark_context_write_csv (struct BenchmarkContext* context, const char* fmt, ...) {
  if (context->format == BENCHMARK_OUTPUT_FORMAT_CSV) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(context->output, fmt, ap);
    va_end(ap);
  }
}

static void
benchmark_timer_start (struct BenchmarkTimer* timer) {
  if (clock_gettime (CLOCK_REALTIME, &(timer->start_wall)) != 0) {
    perror ("Unable to get wall clock time");
    exit (errno);
  }
  if (clock_gettime (CLOCK_PROCESS_CPUTIME_ID, &(timer->start_cpu)) != 0) {
    perror ("Unable to get CPU clock time");
    exit (errno);
  }
}

static void
benchmark_timer_stop (struct BenchmarkTimer* timer) {
  if (clock_gettime (CLOCK_PROCESS_CPUTIME_ID, &(timer->end_cpu)) != 0) {
    perror ("Unable to get CPU clock time");
    exit (errno);
  }
  if (clock_gettime (CLOCK_REALTIME, &(timer->end_wall)) != 0) {
    perror ("Unable to get wall clock time");
    exit (errno);
  }
}

static double
benchmark_timer_elapsed (struct timespec* start, struct timespec* end) {
  return
    (double) (end->tv_sec - start->tv_sec) +
    (((double) (end->tv_nsec - start->tv_nsec)) / 1000000000);
}

static double
benchmark_timer_elapsed_cpu (struct BenchmarkTimer* timer) {
  return benchmark_timer_elapsed (&(timer->start_cpu), &(timer->end_cpu));
}

static double
benchmark_timer_elapsed_wall (struct BenchmarkTimer* timer) {
  return benchmark_timer_elapsed (&(timer->start_wall), &(timer->end_wall));
}

static void
benchmark_codec (SquashCodec* codec, void* data) {
  struct BenchmarkContext* context = (struct BenchmarkContext*) data;
  FILE* compressed = tmpfile ();
  FILE* decompressed = tmpfile ();
  struct BenchmarkTimer timer;

  /* Since we're often running against the source dir, we will pick up
     plugins which have not been compiled.  This should bail us out
     before trying to actually use them. */
  if (squash_codec_init (codec) != SQUASH_OK) {
    return;
  }

  fprintf (stderr, "  %s:%s\n",
           squash_plugin_get_name (squash_codec_get_plugin (codec)),
           squash_codec_get_name (codec));

  if (fseek (context->input, 0, SEEK_SET) != 0) {
    perror ("Unable to seek to beginning of input file");
    exit (-1);
  }

  if (context->first) {
    context->first = false;
  } else {
    benchmark_context_write_json (context, ", ", context->output);
  }

  fputs ("    compressing... ", stderr);
  benchmark_timer_start (&timer);
  squash_codec_compress_file_with_options (codec, compressed, context->input, NULL);
  benchmark_timer_stop (&timer);
  benchmark_context_write_json (context, "{\n        \"plugin\": \"%s\",\n        \"codec\": \"%s\",\n        \"size\": %ld,\n        \"compress_cpu\": %g,\n        \"compress_wall\": %g,\n",
                                squash_plugin_get_name (squash_codec_get_plugin (codec)),
                                squash_codec_get_name (codec),
                                ftell (compressed),
                                benchmark_timer_elapsed_cpu (&timer),
                                benchmark_timer_elapsed_wall (&timer));
  benchmark_context_write_csv (context, "%s,%s,%s,%ld,%ld,%g,%g,",
                               context->input_name,
                               squash_plugin_get_name (squash_codec_get_plugin (codec)),
                               squash_codec_get_name (codec),
                               context->input_size,
                               ftell (compressed),
                               benchmark_timer_elapsed_cpu (&timer),
                               benchmark_timer_elapsed_wall (&timer));
  fputs ("done.\n", stderr);

  if (fseek (compressed, 0, SEEK_SET) != 0) {
    perror ("Unable to seek to beginning of compressed file");
    exit (-1);
  }

  fputs ("    decompressing... ", stderr);
  benchmark_timer_start (&timer);
  squash_codec_decompress_file_with_options (codec, decompressed, compressed, NULL);
  benchmark_timer_stop (&timer);
  benchmark_context_write_json (context, "        \"decompress_cpu\": %g,\n        \"decompress_wall\": %g\n      }",
                                benchmark_timer_elapsed_cpu (&timer),
                                benchmark_timer_elapsed_wall (&timer));
  benchmark_context_write_csv (context, "%g,%g\n",
                               benchmark_timer_elapsed_cpu (&timer),
                               benchmark_timer_elapsed_wall (&timer));
  fputs ("done.\n", stderr);

  fclose (compressed);
  fclose (decompressed);
}

static void
benchmark_plugin (SquashPlugin* plugin, void* data) {
  squash_plugin_foreach_codec (plugin, benchmark_codec, data);
}

int main (int argc, char** argv) {
  struct BenchmarkContext context = { stdout, NULL, NULL, true, 0, BENCHMARK_OUTPUT_FORMAT_JSON };
  bool first_input = true;
  int opt;
  int optc;
  SquashCodec* codec = NULL;

  while ( (opt = getopt(argc, argv, "ho:c:f:")) != -1 ) {
    switch ( opt ) {
      case 'h':
        print_help_and_exit (argc, argv, 0);
        break;
      case 'o':
        context.output = fopen (optarg, "w+");
        if (context.output == NULL) {
          perror ("Unable to open output file");
          return -1;
        }
        break;
      case 'c':
        codec = squash_get_codec ((const char*) optarg);
        if (codec == NULL) {
          fprintf (stderr, "Unable to find codec.\n");
          return -1;
        }
        break;
      case 'f':
        if (strcasecmp ((const char*) optarg, "json") == 0) {
          context.format = BENCHMARK_OUTPUT_FORMAT_JSON;
        } else if (strcasecmp ((const char*) optarg, "csv") == 0) {
          context.format = BENCHMARK_OUTPUT_FORMAT_CSV;
        } else {
          fprintf (stderr, "Invalid output format.\n");
          return -1;
        }
        break;
    }

    optc++;
  }

  if ( optind >= argc ) {
    fputs ("No input files specified.\n", stderr);
    return -1;
  }

  benchmark_context_write_json (&context, "{");
  benchmark_context_write_csv (&context, "Dataset,Plugin,Codec,Uncompressed Size,Compressed Size,Compression CPU Time,Compression Wall Clock Time,Decompression CPU Time,Decompression Wall Clock Time\n");

  while ( optind < argc ) {
    context.input_name = argv[optind];
    context.input = fopen (context.input_name, "r");
    if (context.input == NULL) {
      perror ("Unable to open input data");
      return -1;
    }
    context.first = true;

    if (fseek (context.input, 0, SEEK_END) != 0) {
      perror ("Unable to seek to end of input file");
      exit (-1);
    }
    context.input_size = ftell (context.input);

    fprintf (stderr, "Using %s:\n", context.input_name);
    if (first_input) {
      first_input = false;
      benchmark_context_write_json (&context, "\n");
    } else {
      benchmark_context_write_json (&context, ",\n");
    }

    benchmark_context_write_json (&context, "  \"%s\": {\n    \"uncompressed-size\": %ld,\n    \"data\": [\n      ", context.input_name, context.input_size);

    if (codec == NULL) {
      squash_foreach_plugin (benchmark_plugin, &context);
    } else {
      benchmark_codec (codec, &context);
    }

    
    benchmark_context_write_json (&context, "\n    ]\n  }", context.output);

    optind++;
  }

  benchmark_context_write_json (&context, "\n};\n", context.output);

  return 0;
}
