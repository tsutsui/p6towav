PROG=	p6towav
SRCS=	p6towav.c
OBJS=	${SRCS:.c=.o}

CFLAGS+=	-Wall

${PROG}:	${OBJS}
	${CC} -o ${PROG} ${CFLAGS} ${LDFLAGS} ${OBJS} ${LDLIBS}

clean:
	-rm -f ${PROG} *.o *.core
