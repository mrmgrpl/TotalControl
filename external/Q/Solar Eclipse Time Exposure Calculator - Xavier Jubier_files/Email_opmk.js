<!--
//<![CDATA[
// To avoid address-gathering from spam robots
var Email = "xju" + "bier" + "%40" + "fr" + "ee.fr";

var patternFr = new RegExp("site_pages/");
var patternEn = new RegExp("en/site_pages/");
var patternSolarEclipsesFr = new RegExp("site_pages/solar_eclipses/");
var patternSolarEclipsesEn = new RegExp("en/site_pages/solar_eclipses/");
var patternToursFr = new RegExp("eclipse-city_tours/Voyages_Eclipse_Soleil");
var patternToursEn = new RegExp("eclipse-city_tours/Solar_Eclipse_Tours");
var patternToursDetailFr = new RegExp("eclipse-city_tours/Voyage_Eclipse_Totale_Soleil_");
var patternToursDetailEn = new RegExp("eclipse-city_tours/Total_Solar_Eclipse_");
if ( patternSolarEclipsesEn.test(location.pathname) )
  document.write ( '&copy;&nbsp;<a href="mailto:' + Email + '" class="Bottom">Xavier M. Jubier</a>, 1991-2026<a href="http://xjubier.free.fr/en/site_pages/CopyrightNotice.html" title="Copyright notice..."><sup>*</sup></a> &mdash; IAU member 15159 (Working Group on Solar Eclipses)<br>' );
else if ( patternSolarEclipsesFr.test(location.pathname) )
  document.write ( '&copy;&nbsp;<a href="mailto:' + Email + '" class="Bottom">Xavier M. Jubier</a>, 1991-2026<a href="http://xjubier.free.fr/site_pages/CopyrightNotice.html" title="Droits de reproduction..."><sup>*</sup></a> &mdash; Membre UAI 15159 (Working Groupe de Travail sur les &Eacute;clipses de Soleil)<br>' );
else if ( patternEn.test(location.pathname) )
  document.write ( '&copy;&nbsp;<a href="mailto:' + Email + '" class="Bottom">Xavier M. Jubier</a>, 1991-2026.<a href="http://xjubier.free.fr/en/site_pages/CopyrightNotice.html" title="Copyright notice..."><sup>*</sup></a><br>' );
else if ( patternFr.test(location.pathname) )
  document.write ( '&copy;&nbsp;<a href="mailto:' + Email + '" class="Bottom">Xavier M. Jubier</a>, 1991-2026.<a href="http://xjubier.free.fr/site_pages/CopyrightNotice.html" title="Droits de reproduction..."><sup>*</sup></a><br>' );
else if ( patternToursEn.test(location.pathname) || patternToursDetailEn.test(location.pathname) )
  document.write ( '&copy;&nbsp;<a href="mailto:' + Email + '" class="Bottom">Xavier M. Jubier</a>, 1991-2026 &mdash; IAU member 15159 (Working Group on Solar Eclipses)<br>' );
else if ( patternToursFr.test(location.pathname) || patternToursDetailFr.test(location.pathname) )
  document.write ( '&copy;&nbsp;<a href="mailto:' + Email + '" class="Bottom">Xavier M. Jubier</a>, 1991-2026 &mdash; Membre UAI 15159 (Working Groupe de Travail sur les &Eacute;clipses de Soleil)<br>' );
else
  document.write ( '&copy;&nbsp;<a href="mailto:' + Email + '" class="Bottom">Xavier M. Jubier</a>, 1991-2026.<br>' );
//]]>
//-->