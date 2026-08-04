# Desktop integration: .desktop launcher, scalable icon, AppStream metainfo.
# File names match the GApplication id (cz.ok1br.skimmer_for_linux) so Wayland
# associates the window with its icon.

# The launcher's Exec needs an absolute path: when installed into a prefix
# whose bindir is not on the graphical session's PATH (e.g. a ~/.local user
# install), a bare command name would not launch from the app grid. Bake the
# configured bindir in at build time.
desktop_conf = configuration_data()
desktop_conf.set('bindir', get_option('prefix') / get_option('bindir'))
configure_file(
  input         : 'cz.ok1br.skimmer_for_linux.desktop.in',
  output        : 'cz.ok1br.skimmer_for_linux.desktop',
  configuration : desktop_conf,
  install_dir   : get_option('datadir') / 'applications',
)

install_data('cz.ok1br.skimmer_for_linux.svg',
  install_dir : get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')
install_data('cz.ok1br.skimmer_for_linux.metainfo.xml',
  install_dir : get_option('datadir') / 'metainfo')

# Refresh the icon cache and desktop database at install time so the app and
# its icon show up without requiring a re-login. Automatically skipped when
# DESTDIR is set (distro packaging), leaving those steps to the packager.
gnome.post_install(
  gtk_update_icon_cache   : true,
  update_desktop_database : true,
)
