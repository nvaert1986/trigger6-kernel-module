/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <glob.h>
#include <dlfcn.h>
#include <endian.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdatomic.h>
#define MAGIC 0x314e3654u
#define MAX_W 1920u
#define MAX_H 1200u
#define SLOTS 3
struct packet { uint32_t magic,width,height,stride,sequence,format,length,reserved; };
_Static_assert(sizeof(struct packet)==32,"bridge header must be 32 bytes");
typedef int (*convert_fn)(const uint8_t*,int,uint8_t*,int,uint8_t*,int,int,int);
static atomic_int stopped,reader_done;
static pthread_t reader_thread;
static void wake_handler(int sig) { (void)sig; }
/* A single signal can land between the stopped check and read(), leaving
 * the reader blocked until the next frame (forever on a static desktop).
 * This thread takes SIGTERM/SIGINT (and SIGUSR1 from the writer) via
 * sigwait and keeps interrupting the reader until it has left the loop. */
static void *kick_reader(void *arg)
{
 const sigset_t *set=arg;int sig;
 sigwait(set,&sig);stopped=1;
 while(!reader_done){pthread_kill(reader_thread,SIGUSR2);usleep(20000);}
 return NULL;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9; }
struct slot { uint8_t *data;size_t length;int state; };
struct writer {
 int fd,stop,status;struct slot slots[SLOTS];
 pthread_mutex_t lock;pthread_cond_t ready;pthread_t reader;
 unsigned long sent,coalesced,stale;double write_seconds;
};
static void *write_frames(void *arg)
{
 struct writer *w=arg;sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGTERM);sigaddset(&mask,SIGINT);sigaddset(&mask,SIGUSR1);
 pthread_sigmask(SIG_BLOCK,&mask,NULL);
 pthread_mutex_lock(&w->lock);
 while(!w->stop) {
  int slot=-1;
  for(int i=0;i<SLOTS;i++)if(w->slots[i].state==2)slot=i;
  if(slot<0) { pthread_cond_wait(&w->ready,&w->lock);continue; }
  w->slots[slot].state=3;size_t bytes=w->slots[slot].length;
  pthread_mutex_unlock(&w->lock);
  double begin=now();ssize_t n=write(w->fd,w->slots[slot].data,bytes);int error=errno;
  double elapsed=now()-begin;
  pthread_mutex_lock(&w->lock);w->slots[slot].state=0;w->write_seconds+=elapsed;
  if(n==(ssize_t)bytes)w->sent++;
  else if(n<0 && error==ESTALE)w->stale++;
  else {
   errno=error;perror("bridge write");w->status=1;w->stop=1;stopped=1;
   kill(getpid(),SIGUSR1);
  }
 }
 pthread_mutex_unlock(&w->lock);return NULL;
}
int main(int argc,char **argv)
{
 void *lib=dlopen("libyuv.so.0",RTLD_NOW|RTLD_LOCAL);
 if(!lib)lib=dlopen("libyuv.so",RTLD_NOW|RTLD_LOCAL);
 if(!lib){fprintf(stderr,"libyuv missing: %s\n",dlerror());return 1;}
 convert_fn convert=(convert_fn)dlsym(lib,"ARGBToNV12");
 if(!convert){fprintf(stderr,"ARGBToNV12 missing\n");return 1;}
 glob_t devices={0};const char *path=argc>1?argv[1]:NULL;
 if(!path){if(glob("/dev/trigger6-*-out0-nv12",0,NULL,&devices)||devices.gl_pathc!=1){fprintf(stderr,"Specify exactly one T6 NV12 device\n");return 1;}path=devices.gl_pathv[0];}
 int fd=open(path,O_RDWR|O_CLOEXEC|O_NOFOLLOW);if(fd<0){perror(path);return 1;}
 struct stat st;if(fstat(fd,&st)||!S_ISCHR(st.st_mode)){fprintf(stderr,"Not a character device\n");return 1;}
 const size_t max_raw=MAX_W*MAX_H*4,max_nv12=MAX_W*MAX_H*3/2;
 uint8_t *in=NULL;
 if(posix_memalign((void**)&in,64,sizeof(struct packet)+max_raw)){fprintf(stderr,"Allocation failed\n");return 1;}
 struct writer writer={.fd=fd,.reader=pthread_self(),.lock=PTHREAD_MUTEX_INITIALIZER,.ready=PTHREAD_COND_INITIALIZER};
 for(int i=0;i<SLOTS;i++)if(posix_memalign((void**)&writer.slots[i].data,64,sizeof(struct packet)+max_nv12)){fprintf(stderr,"Allocation failed\n");return 1;}
 /* All threads inherit these blocked; only the reader unblocks SIGUSR2. */
 static sigset_t stop_set;sigset_t all_set;
 sigemptyset(&stop_set);sigaddset(&stop_set,SIGTERM);sigaddset(&stop_set,SIGINT);sigaddset(&stop_set,SIGUSR1);
 all_set=stop_set;sigaddset(&all_set,SIGUSR2);pthread_sigmask(SIG_BLOCK,&all_set,NULL);
 struct sigaction sa={0};sa.sa_handler=wake_handler;sigaction(SIGUSR2,&sa,NULL); /* no SA_RESTART: read() returns EINTR */
 reader_thread=pthread_self();
 pthread_t kicker;if(pthread_create(&kicker,NULL,kick_reader,&stop_set)){fprintf(stderr,"Signal thread failed\n");return 1;}
 pthread_t thread;if(pthread_create(&thread,NULL,write_frames,&writer)){fprintf(stderr,"Writer thread failed\n");return 1;}
 sigset_t wake_set;sigemptyset(&wake_set);sigaddset(&wake_set,SIGUSR2);pthread_sigmask(SIG_UNBLOCK,&wake_set,NULL);
  unsigned long converted=0,skipped=0;double begin=now(),last_report=begin,conversion=0,read_seconds=0,deadline=begin;
 int status=0;
 printf("SIMD pipeline active: %s, target <=60 fps, bounded latest-frame queue\n",path);fflush(stdout);
 while(!stopped){
  double started=now();ssize_t n=read(fd,in,sizeof(struct packet)+max_raw);read_seconds+=now()-started;
  if(n<0){if(errno==EINTR)continue;perror("bridge read");status=1;break;}
  if(n<(ssize_t)sizeof(struct packet)){fprintf(stderr,"Short frame\n");status=1;break;}
  struct packet *input=(struct packet*)in;
  uint32_t w=le32toh(input->width),h=le32toh(input->height),stride=le32toh(input->stride);
  size_t bytes=(size_t)w*h*4;
  if(le32toh(input->magic)!=MAGIC||le32toh(input->format)!=8||!w||!h||w>MAX_W||h>MAX_H||(w&1)||(h&1)||stride!=w*4||le32toh(input->length)!=bytes||n!=(ssize_t)(sizeof(*input)+bytes)){fprintf(stderr,"Invalid input frame\n");status=1;break;}
  int chosen=-1;
  pthread_mutex_lock(&writer.lock);
  for(int i=0;i<SLOTS;i++)if(!writer.slots[i].state){chosen=i;writer.slots[i].state=1;break;}
  pthread_mutex_unlock(&writer.lock);
  if(chosen<0)goto pace;
  uint8_t *out=writer.slots[chosen].data;struct packet *output=(struct packet*)out;
  uint32_t pitch=(w+15u)&~15u;size_t y_size=(size_t)pitch*h,nv12_size=y_size*3/2;
  started=now();
  if(convert(in+sizeof(*input),stride,out+sizeof(*output),pitch,out+sizeof(*output)+y_size,pitch,w,h)){fprintf(stderr,"libyuv conversion failed\n");status=1;break;}
  conversion+=now()-started;converted++;
  *output=*input;output->stride=htole32(pitch);output->format=htole32(6);output->length=htole32(nv12_size);
  pthread_mutex_lock(&writer.lock);
  for(int i=0;i<SLOTS;i++)if(writer.slots[i].state==2){writer.slots[i].state=0;writer.coalesced++;}
  writer.slots[chosen].length=sizeof(*output)+nv12_size;writer.slots[chosen].state=2;
  pthread_cond_signal(&writer.ready);pthread_mutex_unlock(&writer.lock);
pace:
  if(now()-last_report>=30){
   pthread_mutex_lock(&writer.lock);
   printf("elapsed=%.1fs sent=%lu converted=%lu skipped=%lu coalesced=%lu stale=%lu avg_convert_ms=%.3f avg_read_ms=%.3f avg_write_ms=%.3f\n",now()-begin,writer.sent,converted,skipped,writer.coalesced,writer.stale,converted?conversion*1000/converted:0,(converted+skipped)?read_seconds*1000/(converted+skipped):0,writer.sent?writer.write_seconds*1000/writer.sent:0);
   pthread_mutex_unlock(&writer.lock);fflush(stdout);last_report=now();
  }
  deadline+=1./60.;double current=now();if(deadline<current)deadline=current;
  if(deadline>current){struct timespec t={(time_t)deadline,(long)((deadline-(time_t)deadline)*1e9)};clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&t,NULL);}
 }
 reader_done=1;pthread_kill(kicker,SIGUSR1);pthread_join(kicker,NULL);
 pthread_mutex_lock(&writer.lock);writer.stop=1;pthread_cond_broadcast(&writer.ready);pthread_mutex_unlock(&writer.lock);pthread_join(thread,NULL);
 printf("helper stopped: sent=%lu coalesced=%lu status=%d\n",writer.sent,writer.coalesced,status?status:writer.status);
 close(fd);for(int i=0;i<SLOTS;i++)free(writer.slots[i].data);free(in);globfree(&devices);dlclose(lib);return status?status:writer.status;
}
