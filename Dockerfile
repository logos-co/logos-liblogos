# Stage 1: Build
FROM nixos/nix:latest AS builder
RUN echo "experimental-features = nix-command flakes" >> /etc/nix/nix.conf
WORKDIR /app

COPY . /src
RUN nix build /src --out-link ./logos
RUN nix build github:logos-co/logos-package-manager-module#cli --out-link ./package-manager --refresh

RUN mkdir modules \
    && ./package-manager/bin/lgpm --modules-dir ./modules/ install logos-waku-module \
    && ./package-manager/bin/lgpm --modules-dir ./modules/ install logos-storage-module \
    && ln -sf /app/modules/libpq.so /app/modules/libpq.so.5

RUN mkdir /runtime-store \
    && nix-store -qR ./logos | while read path; do \
         cp -a "$path" /runtime-store/; \
       done

RUN mkdir /app-final \
    && cp -rL ./logos /app-final/logos \
    && cp -r ./modules /app-final/modules

# Stage 2: Runtime
FROM debian:bookworm-slim
COPY --from=builder /runtime-store /nix/store
COPY --from=builder /app-final/logos /app/logos
COPY --from=builder /app-final/modules /app/modules
COPY configs/waku_config.json /app/
COPY configs/storage_config.json /app/
WORKDIR /app

CMD ["./logos/bin/logoscore", "-m", "./modules", "--load-modules", "waku_module,storage_module", "-c", "waku_module.initWaku(@waku_config.json)", "-c", "waku_module.startWaku()", "-c", "storage_module.init(@storage_config.json)", "-c", "storage_module.start()", "-c", "storage_module.importFiles('/tmp/storage_files')"]