FROM alpine:edge
RUN apk add --no-cache cmake ninja gcc g++ musl-dev linux-headers
ADD . /src
WORKDIR /src/Debug
RUN env CONTINUOUS_INTEGRATION=1 cmake -GNinja -DNO_SANITIZERS=True -DCMAKE_INSTALL_PREFIX=/dst ..
RUN ninja install

FROM alpine:3.6
COPY --from=0 /dst /
EXPOSE 55555/UDP
CMD ["sockinetd", "-i", "eth0"]
