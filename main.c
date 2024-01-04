#include <SDL2/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <video_file>\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  AVFormatContext *pFormatContext = avformat_alloc_context();
  if (!pFormatContext) {
    fprintf(stderr, "ERROR could not allocate memory for Format Context\n");
    exit(EXIT_FAILURE);
  }

  const char *filename = argv[1];
  if (avformat_open_input(&pFormatContext, filename, NULL, NULL) != 0) {
    fprintf(stderr, "ERROR could not open the file\n");
    exit(EXIT_FAILURE);
  }

  if (avformat_find_stream_info(pFormatContext, NULL) < 0) {
    fprintf(stderr, "ERROR could not get the stream info\n");
    exit(EXIT_FAILURE);
  }

  AVCodec *pCodec = NULL;
  AVCodecParameters *pCodecParameters = NULL;
  int video_stream_index = -1;

  // Loop through the streams and find the first video stream
  for (unsigned int i = 0; i < pFormatContext->nb_streams; i++) {
    AVCodecParameters *pLocalCodecParameters =
        pFormatContext->streams[i]->codecpar;
    AVCodec *pLocalCodec =
        (AVCodec *)avcodec_find_decoder(pLocalCodecParameters->codec_id);

    if (pLocalCodec == NULL) {
      fprintf(stderr, "ERROR unsupported codec!\n");
      continue;
    }

    if (pLocalCodecParameters->codec_type == AVMEDIA_TYPE_VIDEO) {
      if (video_stream_index == -1) {
        video_stream_index = i;
        pCodec = pLocalCodec;
        pCodecParameters = pLocalCodecParameters;
      }

      printf("Video Codec: resolution %d x %d\n", pLocalCodecParameters->width,
             pLocalCodecParameters->height);
    }
  }

  if (video_stream_index == -1) {
    fprintf(stderr, "ERROR could not find video stream\n");
    exit(EXIT_FAILURE);
  }

  AVStream *video_stream = pFormatContext->streams[video_stream_index];
  AVRational framerate = video_stream->r_frame_rate;
  if (framerate.num == 0 || framerate.den == 0) {
    framerate = av_guess_frame_rate(pFormatContext, video_stream, NULL);
  }
  double fps = av_q2d(framerate);
  int delay = (int)(1000 / fps);
  printf("Frame rate: %.3f FPS\n", fps);

  AVCodecContext *pCodecContext = avcodec_alloc_context3(pCodec);
  if (!pCodecContext) {
    fprintf(stderr, "failed to allocated memory for AVCodecContext\n");
    exit(EXIT_FAILURE);
  }

  if (avcodec_parameters_to_context(pCodecContext, pCodecParameters) < 0) {
    fprintf(stderr, "failed to copy codec params to codec context\n");
    exit(EXIT_FAILURE);
  }

  if (avcodec_open2(pCodecContext, pCodec, NULL) < 0) {
    fprintf(stderr, "failed to open codec through avcodec_open2\n");
    exit(EXIT_FAILURE);
  }

  AVFrame *pFrame = av_frame_alloc();
  if (!pFrame) {
    fprintf(stderr, "failed to allocated memory for AVFrame\n");
    exit(EXIT_FAILURE);
  }

  AVFrame *pFrameRGB = av_frame_alloc();
  if (!pFrameRGB) {
    fprintf(stderr, "failed to allocated memory for AVFrameRGB\n");
    exit(EXIT_FAILURE);
  }

  int numBytes = av_image_get_buffer_size(
      AV_PIX_FMT_RGB24, pCodecContext->width, pCodecContext->height, 32);
  uint8_t *buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));

  av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize, buffer,
                       AV_PIX_FMT_RGB24, pCodecContext->width,
                       pCodecContext->height, 32);

  struct SwsContext *sws_ctx = NULL;
  sws_ctx = sws_getContext(pCodecContext->width, pCodecContext->height,
                           pCodecContext->pix_fmt, pCodecContext->width,
                           pCodecContext->height, AV_PIX_FMT_RGB24,
                           SWS_BILINEAR, NULL, NULL, NULL);

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
    fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
    exit(EXIT_FAILURE);
  }

  SDL_Window *win = SDL_CreateWindow(
      "Minimal FFmpeg SDL2 Video Player", SDL_WINDOWPOS_UNDEFINED,
      SDL_WINDOWPOS_UNDEFINED, pCodecContext->width, pCodecContext->height,
      SDL_WINDOW_OPENGL);

  if (!win) {
    fprintf(stderr, "SDL: could not create window - exiting\n");
    exit(EXIT_FAILURE);
  }

  SDL_Renderer *renderer = SDL_CreateRenderer(win, -1, 0);
  SDL_Texture *texture = SDL_CreateTexture(
      renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
      pCodecContext->width, pCodecContext->height);

  AVPacket *pPacket = av_packet_alloc();
  if (!pPacket) {
    fprintf(stderr, "failed to allocated memory for AVPacket\n");
    exit(EXIT_FAILURE);
  }

  int quit = 0;
  SDL_Event e;

  while (!quit) {
    while (SDL_PollEvent(&e) != 0) {
      if (e.type == SDL_QUIT) {
        quit = 1;
      } else if (e.type == SDL_KEYDOWN) {
        if (e.key.keysym.sym == SDLK_ESCAPE) {
          quit = 1;
        }
      }
    }

    if (av_read_frame(pFormatContext, pPacket) < 0) {
      // fprintf(stderr, "End of stream or error reading frame.\n");
      break;
    }

    if (pPacket->stream_index == video_stream_index) {
      int response = avcodec_send_packet(pCodecContext, pPacket);

      if (response < 0) {
        fprintf(stderr, "Error while sending a packet to the decoder: %s\n",
                av_err2str(response));
        continue;
      }

      while (response >= 0) {
        response = avcodec_receive_frame(pCodecContext, pFrame);
        if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
          break;
        } else if (response < 0) {
          fprintf(stderr,
                  "Error while receiving a frame from the decoder: %s\n",
                  av_err2str(response));
          return response;
        }

        if (response >= 0) {
          sws_scale(sws_ctx, (uint8_t const *const *)pFrame->data,
                    pFrame->linesize, 0, pCodecContext->height, pFrameRGB->data,
                    pFrameRGB->linesize);

          SDL_UpdateTexture(texture, NULL, pFrameRGB->data[0],
                            pFrameRGB->linesize[0]);
          SDL_RenderClear(renderer);
          SDL_RenderCopy(renderer, texture, NULL, NULL);
          SDL_RenderPresent(renderer);
        }
      }
      // Delay to control frame rate, adjust delay based on video fps
      SDL_Delay(delay);  // for 30 fps, for example
    }

    av_packet_unref(pPacket);
  }

  av_free(buffer);
  av_frame_free(&pFrameRGB);
  av_frame_free(&pFrame);

  avcodec_close(pCodecContext);

  avformat_close_input(&pFormatContext);

  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(win);
  SDL_Quit();

  return 0;
}
