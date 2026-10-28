PORTNAME=       github-desktop-tui
DISTVERSION=    0.7.0
CATEGORIES=     devel

MAINTAINER=     cleber@bsd.com.br
COMMENT=        Terminal UI Git client built with FTXUI
WWW=            https://github.com/cleberalves40/github-desktop-tui

LICENSE=        MIT
LICENSE_FILE=   ${WRKSRC}/LICENSE

LIB_DEPENDS=    libftxui-component.so:devel/ftxui
RUN_DEPENDS=    git:devel/git \
                gh:devel/gh

USES=           cmake:testing compiler:c++17-lang
USE_GITHUB=     yes
GH_ACCOUNT=     cleberalves40
GH_TAGNAME=     v${DISTVERSION}

PLIST_FILES=    bin/github-desktop-tui

.include <bsd.port.mk>
